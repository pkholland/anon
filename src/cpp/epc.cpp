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
  
    fiber_lock  lock(lookup_fiber_mtx_);
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
    endpoints_.remove_if([](const endpoint& ep)->bool{return ep.lookup_state_ == endpoint::k_invalid;});
  }
  
  // notify anyone who happened to be waiting for the initial lookup to complete
  if (init_lookup_running_) {
    std::unique_lock<std::mutex>  lock(lookup_os_mtx_);
    init_lookup_running_ = false;
    lookup_os_cond_.notify_all();
    lookup_fiber_cond_.notify_all();
  }
}

std::pair<std::vector<http2::hpack_header>, fiber_pipe&>
endpoint_cluster::send_request(const std::vector<http2::hpack_header>& headers)
{
  // in case this is called immediately after the endpoint_cluster is created,
  // fiber-wait until the initial lookup completes
  block_fiber_until_init_lookup_complete();
  
  {
    fiber_lock  lock(lookup_fiber_mtx_);
    
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



