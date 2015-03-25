/*
 Copyright (c) 2015 Anon authors, see AUTHORS file.
 
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
  {
    fiber_lock  lock(mtx_);
    if (shutting_down_)
      return;
    update_running_ = true;
  }

  // call the lookup function to get
  // the list of ip addrs
  auto endpoints = lookup_->lookup();
  
  fiber_lock  lock(mtx_);
  int err = endpoints.first;
  
  if (err != 0) {
  
    // if this lookup failed (for example there was a problem reaching dns),
    // but we previously were able to look things up and so have a non-empty
    // endpoint_ list, then we ignore this error and continue to use the
    // existing list.  But if we currently have no endpoints, for example
    // if this is the first time we are calling lookup, and it fails, then
    // we set this epc into an error state, allowing any fibers waiting in
    // with_connected_pipe to see the error and exit.
    if (endpoints_.empty()) {
      lookup_error_ = err;
      connections_possible_cond_.notify_all();
    }
    
  } else {
  
    // in case we were previously in an error state
    lookup_error_ = 0;

    // for less typing...
    std::vector<std::pair<int,sockaddr_in6>>& eps = endpoints.second;

    total_possible_requests_ = eps.size() * max_conn_per_ep_;
    
    // move all endpoints out of the existing endpoints_ map and
    // into a temporary map that is sorted by ip_addr
    std::map<struct sockaddr_in6,std::unique_ptr<endpoint>> tmp_map;
    for (auto it = endpoints_.begin(); it != endpoints_.end(); ++it)
      tmp_map[it->second->addr_] = std::move(it->second);
      
    // now empty or existing map;
    endpoints_.clear();
    
    // walk through all of the pref/addrs returned by lookup
    int cur_outstanding_requests = 0;
    for (auto ep : eps) {
    
      // is this an existing endpoint?
      auto existing = tmp_map.find(ep.second);
      if (existing != tmp_map.end()) {
      
        // yes, move it back into endpoints_, but use the (possibly updated)
        // preference reported by this call to lookup
        cur_outstanding_requests += existing->second->outstanding_requests_;
        endpoints_.insert( std::make_pair(ep.first, std::move(existing->second)) );
        
      } else {
      
        // no, make a new endpoint and put that in endpoints_
        endpoints_.insert( std::make_pair(ep.first, std::unique_ptr<endpoint>(new endpoint(ep.second))) );
      }
    
    }
    
    // now handle any endpoints that have become "detached" -
    // that is, they were in endpoints_ at the start of this function
    // but were not returned by lookup (it no longer considers them
    // as valid ip_addrs), so they are now still valid pointers
    // in tmp_map
    for (auto it = tmp_map.begin(); it != tmp_map.end(); ++it) {
      if (it->second) {
        it->second->is_detached_ = true;
        endpoints_.insert( std::make_pair(std::numeric_limits<int>::max(), std::move(it->second)) );
      }
    }
    
    cur_avail_requests_ = total_possible_requests_ - cur_outstanding_requests;
    if (cur_avail_requests_ > 0)
      connections_possible_cond_.notify_all();
  }
  
  if (!shutting_down_ && lookup_frequency_seconds_ > 0)  {
    update_task_ = io_dispatch::schedule_task([this]{
      fiber::run_in_fiber([this]
      {
        update_endpoints();
      });
    },cur_time() + lookup_frequency_seconds_);
  }
  
  update_running_ = false;
  update_cond_.notify_one();
}

void endpoint_cluster::backoff(endpoint* ep, const struct timespec& start_time, int explicit_seconds)
{
  #if ANON_LOG_NET_TRAFFIC > 0
  double ts;
  if (explicit_seconds > 0)
    ts = explicit_seconds;
  else
    ts = to_seconds((1 << ep->backoff_exp_) * (cur_time() - start_time));
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
    ep->next_avail_time_ = now + ((1 << (ep->backoff_exp_++)) * error_time);
    if (ep->backoff_exp_ > 13)
      ep->backoff_exp_ = 13;
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
  auto wakeup_task = io_dispatch::schedule_task([this]{
    fiber::run_in_fiber([this]{
      fiber_lock  lock(mtx_);
      connections_possible_cond_.notify_all();
      auto ct = cur_time();
      auto it = io_retry_tasks_.begin();
      while (it != io_retry_tasks_.end() && it->first <= ct) {
        io_dispatch::remove_task(it->second);
        io_retry_tasks_.erase(it++);
      }
    });
  },ep->next_avail_time_);
  io_retry_tasks_.insert( std::make_pair(ep->next_avail_time_, wakeup_task) );

  // this exception gets caught in with_connected_pipe and causes that
  // to call us (do_with_connected_pipe) again immediately.  But we have
  // set this ip_addr's next_avail_time_ out some amount, meaning
  // that if this epc has other available ip_addrs those will be tried.
  // If no other ip_addrs are available then the function will wait
  // in connections_possible_cond_.wait (above in this routine) until
  // one of the io_dispatch scheduled tasks (immediately above) notifies
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
  // throwing io errors on read and write
  while (true) {
  
    // when we exit the try_again loop
    // we will have set these 2 to the
    // socket/pipe thing we are trying
    // to use when calling 'caller'
    endpoint* ep = 0;
    std::list<std::unique_ptr<endpoint::sock>>::iterator  s_it;
  
    // this next loop deals with the inability to find
    // a currently usable ip_addr associated with this
    // epc.  Its important steps are to 1) lock mtx_,
    // 2) wait until cur_avail_requests_ > 0, and
    // then 3) try to find/connect to an ip_addr
    bool  try_again = true;
    while (try_again) {
      fiber_lock  lock(mtx_);
          
      // sleep until it is worth trying a connection
      while (cur_avail_requests_ == 0 && lookup_error_ == 0)
        connections_possible_cond_.wait(lock);
        
      // if the lookup has failed, then throw.  This
      // transfers the error seen in the update_endpoints
      // fiber to this one.
      if (lookup_error_ != 0) {
        errno = lookup_error_;
        do_error("endpoint_cluster lookup");
      }
        
      // if we are shutting down then we abort these
      // calls
      if (shutting_down_)
        throw std::runtime_error("endpoint_cluster destructed while fiber waiting for available endpoints, aborting call to with_connected_pipe");
        
      // backoff etc will mark certain ip_addrs as
      // temporarily unavailable using a timestamp,
      // so get the current time so we can compare
      auto ct = cur_time();
      
      // figure out which endpoint/ip_addr we are going to use.
      // the 'endpoints_' list is in order, sorted by preference,
      // so we will use the first one that is currently usable
      for (auto ep_it = endpoints_.begin(); ep_it != endpoints_.end(); ep_it++) {
      
        // just for less typing...
        ep = ep_it->second.get();
        
        // we can only use this ep if:
        //  1) it isn't already at the maximum number of allowed outstanding requests
        //  2) it isn't in a "detached" state, and
        //  3) we haven't marked it as temporarily unavailable
        if ( ep->outstanding_requests_ < max_conn_per_ep_
          && !ep->is_detached_
          && ep->next_avail_time_ < ct) {
        
          // Round Robin
          // how many ip_addrs have this same preference?
          auto ep2_it = ep_it;
          ++ep2_it;
          unsigned int num_with_same_pref = 1;
          while ((ep2_it != endpoints_.end()) && (ep2_it->first == ep_it->first)) {
          
            // ignoring any that we can't use at the moment
            if (!ep2_it->second->is_detached_
              && (ep2_it->second->outstanding_requests_ < max_conn_per_ep_)
              && (ep2_it->second->next_avail_time_ < ct))
              ++num_with_same_pref;
              
            ++ep2_it;
          }
          // if more than one, reset ep to the next round-robin ip_addr
          if (num_with_same_pref > 1) {
            unsigned int which = round_robin_index_++ % num_with_same_pref;
            for (unsigned int i = 0; i < which; i++) {
              ++ep_it;
              
              // remember to skip the ones we ignored above when we were counting
              while (!(  !ep_it->second->is_detached_
                      && (ep_it->second->outstanding_requests_ < max_conn_per_ep_)
                      && (ep_it->second->next_avail_time_ < ct)))
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
            if (!(*s_it)->in_use_)
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
            // we remove the in_use settings and then re-lock before exiting
            // if that were to happen.
            mtx_.unlock();
            std::pair<int, std::unique_ptr<fiber_pipe>> con;
            
            // TODO, start a timer to detect "suspiciously slow" connection attempts.
            // probably some research into what linux actually does with unanswered
            // SYN segments, and the frequency of resends for them.  Whatever happens
            // here should compliment that linux behavior, not simply reimplement it.
            
            try {
              con = tcp_client::connect((struct sockaddr *)&ep->addr_, ep->addr_.sin6_family == AF_INET6 ?
                                            sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in));
            } catch (...) {
              mtx_.lock();
              ep_exit(ep);
              throw;
            }
            mtx_.lock();
            
            if (shutting_down_) {
              ep_exit(ep);
              throw std::runtime_error("endpoint_cluster destructed while fiber waiting on tcp_client::connect, aborting call to with_connected_pipe");
            }
            
            switch (con.first) {  // con.first is the error code part of the return value
            
              case 0: {           // connected, we're good to go!
                auto ns = new endpoint::sock(std::move(con.second));
                ep->socks_.push_back(std::unique_ptr<endpoint::sock>(ns));
                s_it = ep->socks_.end();
                --s_it;
             }  break;
              
                          
              //  Here is a basic description of the three interesting, similar
              //  errors that we care about that can come back from connect.
              //
              //  ENETUNREACH:  no SYN segment was answered by the server, and at least
              //                one such segment generated an ICMP "destination unreachable"
              //                response in some intermediate router (note that connect will send
              //                more than one SYN if it doesn't hear back).  Allocated, but currently
              //                unassigned ip addresses can cause this.  These are more common in
              //                ipv6 than ipv4.
              //
              //  ETIMEDOUT:    no SYN segment was answered by the server or intermediate routers.
              //                We received no response whatsoever to any of our SYN segments.
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
                ep_exit(ep);
                backoff(ep, ct);  // ends with a "throw backoff_error();"
              
              // current decision is to not special-case this error code.  It would represent
              // something like a very poorly configured machine, or perhaps some kind of resource
              // leak in our logic.  Neither of these are things we can easily account for here.
              //case EAGAIN:        // we ran out of available (local) port numbers, so couldn't do the "bind" step
              
              default:            // some unknown problem
                ep_exit(ep);
                errno = con.first;
                do_error("tcp_client::connect(" << ep->addr_ << ")");
            }
            
          } else { // end of "if (s_it == ep->end())"
          
            // here we found an unused, already-connected socket, so use it.
            (*s_it)->in_use_ = true;
          
          } // end of else of "if (s_it == ep->end())"
          
          // ep, and s_it are now set
          
          try_again = false;  // so we exit the "while (try_again)" loop after we...
          break;              // break out of the "for (auto ep_it = endpoints_.begin(); ep_it != endpoints_.end(); ep_it++)" loop
          
        } // end of "if (ep->outstanding_requests_ < max_conn_per_ep_)"
        
      } // end of "for (auto ep_it = endpoints_.begin(); ep_it != endpoints_.end(); ep_it++)"
      
    } // end of "while (try_again)"
    
    // mtx_ is now unlocked
    // ep and s_it are set pointing to the ip_addr/socket/fiber_pipe we are supposed to try.
    // if a read/write exception (fiber_io_error) goes off at this point we immediately
    // close our side of the socket, deleting the corresponding 'sock' object and retry
    // this function.  By default this condition does _not_ cause us to back off of trying
    // this ip_addr.  However, fiber_io_error has a public bool "backoff" which, if set,
    // will cause this us to backoff.  The idea is that code which might do something like
    // parse an HTTP response, looking for a 503, could throw a specially constructed
    // fiber_io_error to trigger this behavior.  But normally, a simple error in attempting
    // to read from or write to a once-valid socket will just assume that the other side
    // closed its end, and so we are free to immediately attempt to reconnect.
    
    try {
      caller->exec((*s_it)->pipe_.get());
      fiber_lock  lock(mtx_);
      ep->backoff_exp_ = 0;
      ep_exit(ep);
      (*s_it)->in_use_ = false;
      return;
    }
    catch(const fiber_io_error& e)
    {
      fiber_lock  lock(mtx_);
      ep_exit(ep);
      ep->socks_.erase(s_it);
      if (e.backoff_)
        backoff(ep, timespec(), e.backoff_seconds_);
    }
    catch(...)
    {
      fiber_lock  lock(mtx_);
      ep_exit(ep);
      (*s_it)->in_use_ = false;
      throw;
    }
    
  } // end of "while (true)", but we do the normal return when the 'try' clause above does not throw
  
}


