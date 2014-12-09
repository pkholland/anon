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

#pragma once

#include "http2.h"
#include <vector>

class endpoint_cluster
{
public:
  template<typename LookupFn>
  endpoint_cluster(LookupFn lookup)
    : lookup_(new lookup_caller_t<LookupFn>(lookup)),
      init_lookup_running_(true),
      failed_lookup_attempts_(0)
  {
    fiber::run_in_fiber([this]{update_endpoints();});
  }
  
  ~endpoint_cluster()
  {
    if (get_current_fiber_id() == 0)
      block_os_thread_until_init_lookup_complete();
    else
      block_fiber_until_init_lookup_complete();
  }
  
  void block_os_thread_until_init_lookup_complete()
  {
    anon::unique_lock<std::mutex>  lock(lookup_os_mtx_);
    while (init_lookup_running_)
      lookup_os_cond_.wait(lock);
  }
  
  void block_fiber_until_init_lookup_complete()
  {
    fiber_lock  lock(lookup_fiber_mtx_);
    while (init_lookup_running_)
      lookup_fiber_cond_.wait(lock);
  }
  
  // send 'headers' to one of the ip addrs of this endpoint_cluster,
  // suspend this fiber until a response is returned, and return the
  // response
  std::pair<std::vector<http2::hpack_header>, fiber_pipe&> send_request(const std::vector<http2::hpack_header>& headers);
  
private:
  std::mutex              lookup_os_mtx_;
  std::condition_variable lookup_os_cond_;
  fiber_mutex             lookup_fiber_mtx_;
  fiber_cond              lookup_fiber_cond_;
  bool                    init_lookup_running_;
  int                     failed_lookup_attempts_;

  struct lookup_caller
  {
    virtual ~lookup_caller() {}
    virtual std::pair<int, std::vector<sockaddr_in6>> lookup() = 0;
  };
  
  template<typename Fn>
  struct lookup_caller_t : public lookup_caller
  {
    lookup_caller_t(Fn f) : f_(f) {}
    virtual std::pair<int, std::vector<sockaddr_in6>> lookup() { return f_(); }
    Fn f_;
  };
  
  std::auto_ptr<lookup_caller>  lookup_;
  
  void update_endpoints();
  
  struct endpoint
  {
    endpoint(const struct sockaddr_in6 &addr)
      : lookup_state_(k_invalid),
        outstanding_requests_(0)
    {
      size_t addrlen = (addr.sin6_family == AF_INET6) ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
      memcpy(&addr_, &addr, addrlen);
      next_avail_time_.tv_sec = std::numeric_limits<time_t>::min();
      next_avail_time_.tv_nsec = 0;
    }
    
    endpoint(endpoint&& other)
      : addr_(other.addr_),
        next_avail_time_(other.next_avail_time_),
        lookup_state_(other.lookup_state_),
        connect_status_(other.connect_status_),
        outstanding_requests_(other.outstanding_requests_),
        sync_(std::move(other.sync_))
    {}
    
    ~endpoint()
    {
      // fancy "tell h2 to shutdown, then wait until fiber exits"
    }
    
    struct sync
    {
      fiber_cond  cond_;
      fiber_mutex mutex_;
      int         connect_status_;
      std::unique_ptr<fiber_pipe> pipe_;
      fiber       fiber_;
      
      template<typename Fn>
      sync(Fn f)
        : fiber_(f),
          connect_status_(k_connecting)
      {}
      
      enum {
        k_connecting,
        k_connected,
        k_failed
      };
    };
      
    enum {
      k_invalid,
      k_valid,
    };
    
    struct sockaddr_in6     addr_;
    struct timespec         next_avail_time_;
    int                     lookup_state_;
    int                     connect_status_;
    int                     outstanding_requests_;
    std::unique_ptr<sync>   sync_;
  };
  
  std::list<endpoint> endpoints_;

};

