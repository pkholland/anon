/*
 Copyright (c) 2014 Anon authors, see AUTHORS file.
 
 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:
 
 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.
 
 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
*/

#include "epc.h"
#include "tcp_client.h"
#include <netdb.h>

void endpoint_cluster::update_endpoints()
{
  auto endpoints = lookup_->lookup();
  
  int err = endpoints.first;
  if (err != 0) {
  
    #if ANON_LOG_NET_TRAFFIC > 1
    anon_log("lookup failed with: " << (err > 0 ? error_string(err) : gai_strerror(err)));
    #endif
    ++failed_lookup_attempts_;
    
  } else {
  
    fiber_lock  lock(mtx_);
    failed_lookup_attempts_ = 0;
 
    std::vector<sockaddr_in6>& eps = endpoints.second;
    // mark the lookup_state_ of each endpoint as invalid
    for (auto it = endpoints_.begin(); it != endpoints_.end(); it++)
      it->lookup_state_ = endpoint::k_invalid;
     
    // walk through all of the addresses returned by lookup
    for (auto addr : eps) {
    
      // did we already know about this one?
      bool exists = false;
      for (auto it = endpoints_.begin(); it != endpoints_.end(); it++) {
        if (it->addr_ == addr) {
          it->lookup_state_ = endpoint::k_valid;
          exists = true;
          break;
        }
      }

      // if this is a new one add it to our list
      if (!exists)
        endpoints_.push_back(endpoint(addr));      
    }
    
    // remove each element of endpoints_ that is no longer in eps (still has its state_ set to invalid)
    // and is not currently in use.  Currently in use ones will be removed when the last user of them
    // returns
    endpoints_.remove_if([](const endpoint& ep)->bool
    {
      return ep.lookup_state_ == endpoint::k_invalid && ep.outstanding_requests_ == 0;
    });
  }
  
  // notify anyone who happened to be waiting for the initial lookup to complete
  if (init_lookup_running_) {
    std::unique_lock<std::mutex>  lock(lookup_os_mtx_);
    init_lookup_running_ = false;
    lookup_os_cond_.notify_all();
    lookup_fiber_cond_.notify_all();
  }
}

void endpoint_cluster::backoff(endpoint* ep, const struct timespec& start_time, int explicit_seconds)
{
  #if ANON_LOG_NET_TRAFFIC > 0
  double ts;
  if (explicit_seconds > 0)
    ts = explicit_seconds;
  else
    ts = to_seconds((1 << (ep->backoff_exp)) * (cur_time() - start_time));
  anon_log("backing off of: " << ep->addr_ << ", for the next " << ts << (explicit_seconds > 0 ? " (explicit)" : "") << " seconds");
  #endif

  auto now = cur_time();
  if (explicit_seconds > 0)
    ep->next_avail_time_ = now + explicit_seconds;
  else {
    auto error_time = now - start_time;

    // for each time we get an error trying to connect or talk to this ip_addr,
    // double the amount of time we will wait before attempting to connect
    // to it again.
    ep->next_avail_time_ = now + (1 << (ep->backoff_exp++)) * error_time;
    if (ep->backoff_exp > 13)
      ep->backoff_exp = 13;
  }

  // schedule a task to call connections_possible_cond_.notify_all()
  // when we have allowed enough time to pass for it to be worth
  // trying this ip_addr again.
  //
  // note that fiber_cond::notify_all can only be called in a fiber,
  // and the task mechanism in io_dispatch does not call our functor
  // in a fiber.  So here the functor creates a fiber and executes
  // notify_all in there.  It is the fiber exiting condition that
  // actually schedules the notified fibers to run.
  auto wakeup_task = io_dispatch::schedulel_task([this]{
    fiber::run_in_fiber([this]{
      fiber_lock  lock(mtx_);
      connections_possible_cond_.notify_all();
      auto ct = cur_time();
      auto it = io_retry_tasks_.begin();
      while (it != io_retry_tasks_.end() && it->first <= ct) {
        id_dispatch::remove_task(it->second);
        io_retry_tasks_.erase(it);
        it = io_retry_tasks_.begin();
      }
    });
  },ep->next_avail_time_);
  io_retry_tasks_[ep->next_avail_time_] = wakeup_task;

  // this exception gets caught in with_connected_pipe and causes that
  // to call us (do_with_connected_pipe) again immediately.  But we have
  // set this ip_addr's next_avail_time_ out some amount, meaning
  // that if this epc has other available ip_addrs those will be tried.
  // If no other ip_addrs are available then the function will wait
  // in connections_possible_cond_.wait (above in this routine) until
  // one of the id_dispatch scheduled tasks (immediately above) notifies
  // of the possibility of reconnecting.  If this epc has only one ip_addr
  // this will execute a fairly traditional "exponential backoff" design.
  throw backoff_error();
}


// a bit complicated because of all of the error/retry things it deals with...
void endpoint_cluster::do_with_connected_pipe(tcp_caller* caller)
{
  // ensure that we delete this when we exit
  std::unique_ptr<tcp_caller> c(caller);
    
  // this outer loop performs reconnects to an existing,
  // working ip_addr when a once-functional socket is now
  // throw io errors on read and write
  while (true) {
  
    // when we exit the try_again loop
    // we will have set these 2 to the
    // socket/pipe thing we are trying
    // to use when calling 'caller'
    endpoint* ep = 0;
    std::list<std::unique_ptr<endpoint::sock>>::iterator  s_it;
  
    // this next loop deals with the inability to find
    // a currently usable ip_addr associated with this
    // epc.  It's important steps are to 1) lock mtx_
    // and 2) wait until cur_avail_requests_ > 0, and
    // then 3) try to find/connect to an ip_addr
    bool  try_again = true;
    while (try_again) {
      fiber_lock  lock(mtx_);
          
      // sleep until it is worth trying a connection
      while (cur_avail_requests_ == 0)
        connections_possible_cond_.wait(mtx_);
        
      // backoff etc will mark certain ip_addrs as
      // temporarily unavailable, so get the current
      // time so we can compare to those ip_addrs
      auto ct = cur_time();
      
      // figure out which endpoint (ip_addr) we are going to use.
      // the 'endpoints_' list is in order, sorted by preference, so
      // we will use the first one that is currently usable
      for (auto ep_it = endpoints_.begin(); ep_it != endpoints_.end(); ep_it++) {
      
        // just for less typing...
        ep = ep_it->second.get();
        
        // we can only use this ep if it isn't already at the maximum
        // number of allowed outstanding requests and we haven't marked it
        // as temporarily unavailable
        if (ep->outstanding_requests_ < max_conn_per_ep_ && ep->next_avail_time_ < ct) {
        
          // Round Robin
          // how many ip_addrs have this same preference?
          // (ignoring any that we can't use at the moment)
          auto ep2_it = ep_it;
          ++ep2_it;
          unsigned int num_with_same_pref = 1;
          while ((ep2_it != endpoints_.end()) 
              && (ep2_it->first == ep_it->first)  // same "preference"
              && (ep2_it->second->outstanding_requests_ < max_conn_per_ep_)
              && (ep2_it->next_avail_time_ < ct)) {
            ++num_with_same_pref;
            ++ep2_it;
          }
          // if more than one, reset ep to the next round-robin ip_addr
          if (num_with_same_pref > 1) {
            unsigned int which = round_robin_index_++ % num_with_same_pref;
            for (unsigned int i = 0; i < which; i++) {
              ++ep_it;
              
              // remember to ignore the already-full and not-yet-available ones
              while (!(ep_it->second->outstanding_requests_ < max_conn_per_ep_)
                  || !(ep_it->second->next_avail_time_ < ct))
                ++ep_it;
            }
            // reset ep to the one we are actually going to use
            ep = ep_it->second.get();
          }
        
          // we're going to make a request to this endpoint
          ++ep->outstanding_requests_;
          
          // which will decrement the total number of available requests
          --cur_avail_requests_;
        
          // is there already an existing, unused socket connected to this ip_addr?
          for (s_it = ep->socks_.begin(); s_it != ep->socks_.end(); s_it++)  {
            if (!s_it->in_use_)
              break;
          }
          
          // if no existing, unused sockets then connect a new one
          if (s_it == ep->socks_.end()) {
          
            // sleep this fiber until the connection attempt completes.  The
            // return value is a pair<int err_code, unique_ptr<fiber_pipe>>
            //
            // note that we do a manual unlock followed by re-lock because we
            // don't want the mutex locked over this call to connect.  But we
            // want to run in a single stack frame so that the exception handling
            // is simpler.  Alternate designs would be to run the connect
            // completion code in a fiber, but then we would need to replicate
            // some exception handling logic, and it would be harder to do there.
            // We are being somewhat careful here to mark all data structures
            // we are referencing as being "used" prior to unlocking, and this
            // ensures that they won't be deleted or reused before we get back
            // and lock again.  And because connect can throw (for example, if
            // we run out of file descriptors or something similar) we ensure
            // we remove the in_use settings and then re-locks before exiting
            // if that were to happen.
            mtx_.unlock();
            std::pair<int, std::unique_ptr<fiber_pipe>> con;
            
            // TODO, start a timer to detect "suspiciously slow" connection attempts
            
            try {
              con = tcp_client::connect((struct sockaddr *)&ep->addr_, ep->addr_.sin6_family == AF_INET6 ?
                                            sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in));
            } catch (...) {
              mtx_.lock();
              ep_exit(ep);
              throw;
            }
            mtx_.lock();
            
            switch (con.first) {  // con.first is the error code part of the return value
            
              case 0:             // connected, we're good to go!
                fp = con.second.get();
                auto ns = new sock(std::move(con.second));
                ep->socks_.push_back(std::unique_ptr<sock>(ns));
                s_it = ep->socks_.end();
                --s_it;
                break;
              
              //  
              // the set of errors we treat with specialized code:
              //
                          
              //  Here is a basic description of the three interesting, similar
              //  errors that can come back from connect.
              //
              //  ENETUNREACH:  no SYN segment was answered by the server, and at least
              //                one such segment generated an ICMP "destination unreachable"
              //                response in some intermediate router (note that connect will send
              //                more than one SYN if it doesn't hear back).  Allocated, but currently
              //                unassigned ip addresses can cause this.  These are more common in
              //                ipv6 than ipv4.
              //
              //  ETIMEDOUT:    no SYN segment was answered by the server or intermediate routers.
              //                We received no response whatsoever to any of our SYNs.
              //
              //  ECONNREFUSED: server responded to our SYN with a RST.  Generally indicates that
              //                the server machine/os is running and reachable, but that no server
              //                process is listening on the port number we are requesting.  Intermediate
              //                load balancers and proxies greatly complicate this picture, because
              //                in those cases it is the intermediate machine that is acting like
              //                the server from the perspective of these network errors.
              //
              //  Particularly because of the last point in ECONNREFUSED, where proxies complicate the
              //  issues of understanding what each error value actually implies about the state of the
              //  network and servers on it, we don't attempt to treat any of these errors differently
              //  from each other.  We simply adopt a basic strategy of backing off of our attempt to
              //  connect to that endpoint.  We take into account the length of time it takes to get the
              //  error back in such a way that we more quickly back off of ip_addrs that return errors
              //  more slowly.
              
              case ENETUNREACH:
              case ECONNREFUSED:
              case ETIMEDOUT:
                backoff(ep, ct);  // ends with a throw backoff_error
              
              // current decision is to not special-case this error code.  It would represent
              // something like a very poorly configured machine, or perhaps some kind of resource
              // leak in our logic.  Neither of these are things we can easily account for here.
              //case EAGAIN:        // we ran out of available (local) port numbers, so couldn't do the "bind" step
              
              default:            // some unknown problem, let with_connected_pipe terminate
                errno = con.first;
                do_error("tcp_client::connect(" << ep->addr_ << ")");
            }
            
          } else { // end of "if (s_it == ep->end())"
          
            // here there is an unused, already-connected socket, so use it.
            fp = s_it->pipe_.get();
            s_it->in_use = true;
          
          } // end of else of "if (s_it == ep->end())"
          
          // ep, fp, and in_use are now set
          
          try_again = false;  // so we exit the "while (try_again)" loop after we...
          break;              // break out of the "for (auto ep_it = endpoints_.begin(); ep_it != endpoints_.end(); ep_it++)" loop
          
        } // end of "if (ep->outstanding_requests_ < max_conn_per_ep_)"
        
      } // end of "for (auto ep_it = endpoints_.begin(); ep_it != endpoints_.end(); ep_it++)"
      
    } // end of "while (try_again)"
    
    // mtx_ is now unlocked
    // ep and s_it are set pointing to the ip_addr/socket/fiber_pipe we are supposed to try.
    // if a read/write exception (fiber_io_error) goes off at this point we immediately
    // close our side of the socket, deleting the corresponding 'sock' object and retry
    // the routine.  By default this condition does _not_ cause us to back off of trying
    // this ip_addr.  However, fiber_io_error has a public bool "backoff", which if set
    // will cause this us to backoff.  The idea is that code which might do something like
    // parse an HTTP response, looking for a 503 could throw a specially constructed
    // fiber_io_error to trigger this behavior.  But normally, a simple error in attempting
    // to read from or write to a once-valid socket will just assume that the other side
    // closed its end, and so we are free to immediately attempt to reconnect.
    
    try {
      caller->exec(0/*err_code*/, s_it->pipe_.get());
      fiber_lock  lock(mtx_);
      ep->backoff_exp = 0;
      ep_exit(ep);
      s_it->in_use = false;
      return;
    }
    catch(const fiber_io_error& e)
    {
      fiber_lock  lock(mtx_);
      ep_exit(ep);
      ep->socks_.erase(s_it);
      if (e.backoff)
        backoff(ep, ct, e.backoff_seconds);
    }
    catch(...)
    {
      fiber_lock  lock(mtx_);
      ep_exit(ep);
      s_it->in_use = false;
      throw;
    }
    
  } // end of "while (true)", but we do the normal return when the 'try' clause above does not throw
  
}


#if 0
std::pair<std::vector<http2::hpack_header>, fiber_pipe&>
endpoint_cluster::send_request(const std::vector<http2::hpack_header>& headers)
{
  // in case this is called immediately after the endpoint_cluster is created,
  // fiber-wait until the initial lookup completes
  block_fiber_until_init_lookup_complete();
  
  {
    fiber_lock  lock(mtx_);
    
    // find the first addr with no outstanding requests (if there is one)
    endpoint* ep = 0;
    for (auto it = endpoints_.begin(); it != endpoints_.end(); it++) {
      if (it->outstanding_requests_ == 0 && it->connect_status_ != endpoint::k_invalid) {
        ep = &(*it);
        break;
      }
    }
    
    if (ep != 0) {
      // it's still possible that this is the first time we have used this
      // endpoint, so need to connect and start things up
      ++ep->outstanding_requests_;
      if (!ep->sync_) {
        endpoint::sync* sync = (endpoint::sync*)operator new(sizeof(endpoint::sync));
        ep->sync_ = std::unique_ptr<endpoint::sync>(new (sync) endpoint::sync([ep,sync]{
        
          auto sz = ep->addr_.sin6_family == AF_INET6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
          auto c = tcp_client::connect((struct sockaddr*)&ep->addr_, sz);
          fiber_lock lock(sync->mutex_);
          if (c.first == 0) {
            sync->connect_status_ = endpoint::sync::k_connected;
            sync->pipe_ = std::move(c.second);
          } else {
            sync->connect_status_ = endpoint::sync::k_failed;
          }
          sync->cond_.notify_all();
          
          
        }));
      }
      
      fiber_lock lock(ep->sync_->mutex_);
      while (ep->sync_->connect_status_ == endpoint::sync::k_connecting)
        ep->sync_->cond_.wait(lock);
    }
  }
}
#endif



