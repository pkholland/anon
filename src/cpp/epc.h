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

#include "tcp_client.h"

class endpoint_cluster
{
public:
  template<typename LookupFn>
  endpoint_cluster(LookupFn lookup, int max_conn_per_ep = 20)
    : lookup_(new lookup_caller_t<LookupFn>(lookup)),
      shutting_down_(false),
      max_conn_per_ep_(max_conn_per_ep),
      round_robin_index_(0),
      cur_avail_requests_(0),
      total_possible_requests_(0)
  {
    fiber::run_in_fiber([this]{update_endpoints();});
  }
  
  ~endpoint_cluster()
  {
    // kill all outstanding io retries
    for (auto it = io_retry_tasks_.begin(); it != io_retry_tasks_.end(); it++)
      io_dispatch::remove_task(it->second);
      
    shutting_down_ = true;
    if (get_current_fiber_id() == 0)
      block_os_thread_until_all_requests_complete();
    else
      block_fiber_until_all_requests_complete();
  }
  
  void block_fiber_until_all_requests_complete()
  {
    fiber_lock  lock(mtx_);
    while (cur_avail_requests_ < total_possible_requests_)
      zero_outstanding_requests_cond_.wait(lock);
  }
  
  void block_os_thread_until_all_requests_complete()
  {
    std::mutex              mtx;
    std::condition_variable cond;
    bool                    running = true;
    try {
      fiber::run_in_fiber([this, &mtx, &cond, &running]{
        block_fiber_until_all_requests_complete();
        std::lock<std::mutex> lock(mtx);
        running = false;
        cond.notify_all();
      });
      std::lock<std::mutex> lock(mtx);
      while (running)
        cond.wait(lock);
    } catch (...)
    {}
  }
  
  // call the given 'f' with an error code and pipe.
  // The signature of 'f' must be:
  //
  //    void f(int err_code, const fiber_pipe* pipe)
  //
  // if err_code is unequal to 0 then 'pipe' will be 0 and err_code
  // will represent the reason for failure to connect to any of the
  // endpoints associated with this endpoint_cluster.  If err_code
  // is > 0 then it represents a system errno code.  If it is < 0
  // then it represents "GetAddrInfo" code which can be displayed
  // in human-readable form by calling the system call gai_strerror.
  //
  // Note, 'f' will be called at most once with err_code != 0.  If
  // it is called with err_code != 0 'f' will not be called again
  // after the call with err_code != 0.  But 'f' may be called
  // multiple times with err_code == 0.  When 'f' is called with
  // err_code == 0 'pipe' will contain a connected socket. But it is possible
  // that the other side of this socket will already be in a bad state
  // or will enter a bad state while 'f' is executing -- presumably
  // attempting to write to, or read from 'pipe'.  These failed reads
  // and writes result in exceptions being thrown, and when those
  // exceptions are caught the mechanism will generally attempt to
  // reconnect, followed by calling 'f' again (with a new 'pipe').
  // Because of this behavior it is generally a good idea to perform
  // your significant calculations to compute what you wish to write
  // to the pipe prior to calling with_connected_pipe, and then mostly
  // perform the write itself (perhaps followed by read) inside of 'f'.
  //
  // Also, if 'f' is called, regadless of the value of err_code, and 'f'
  // completes without throwing an exception (it returns normally)
  // then 'f' will not be called again.
  //
  // with_connected_pipe returns true if 'f' was called with err_code == 0
  // and 'f' did not throw and exception during that call.  Otherwise
  // 'f' will have been called with err_code != 0 and with_connected_pipe 
  // will return false.
  template<typename Fn>
  bool with_connected_pipe(Fn f)
  {
    while (true) {
      try {
        do_with_connected_pipe(new tcp_call<F>(f));
        return true;
      }
      catch(const backoff_error&)   // retry these
      {
        // unless we are shutting down
        if (shutting_down_)
          return false;
      }
      catch(...)
      {
        try {
          // ensure this gets called with an err code
          f(errno != 0 ? errno : ENOMEM, 0);
        }
        catch(...)
        {}
        return false;
      }
    }
  }
  
  // send 'headers' to one of the ip addrs of this endpoint_cluster,
  // suspend this fiber until a response is returned, and return the
  // response
//  std::pair<std::vector<http2::hpack_header>, fiber_pipe&> send_request(const std::vector<http2::hpack_header>& headers);
  
private:
  fiber_mutex   mtx_;
  fiber_cond    zero_outstanding_requests_cond_;
  fiber_cond    connections_possible_cond_;
  int           failed_lookup_attempts_;
  int           max_conn_per_ep_;
  unsigned int  round_robin_index_;
  int           cur_avail_requests_;
  int           total_possible_requests_;
  
  std::multimap<struct timespec,id_dispatch::scheduled_task>  io_retry_tasks_;

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
  
  struct tcp_caller
  {
    virtual ~tcp_caller() {}
    virtual void exec(int err_code, const fiber_pipe* pipe) = 0;
  };

  template<typename Fn>
  struct tcp_call : public tcp_caller
  {
    tcp_call(Fn f)
      : f_(f)
    {}
  
    virtual void exec(int err_code, const fiber_pipe* pipe)
    {
      f_(err_code, pipe);
    }
    
    Fn f_;
  };
  
  void do_with_connected_pipe(tcp_caller* caller);
    
  void update_endpoints();
  
  // each 'endpoint' is a single ip address
  // and a collection of 1 or more (up to max_conn_per_ep_)
  // open, connected sockets to that endpoint
  struct endpoint
  {
    endpoint(const struct sockaddr_in6 &addr)
      : outstanding_requests_(0)
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
    
    struct sock
    {
      sock(std::unique_ptr<fiber_pipe>&& pipe)
        : pipe_(std::move(pipe)),
          in_use_(true)
      {}
      
      std::unique_ptr<fiber_pipe> pipe_;
      bool                        in_use_;
    };
      
    enum {
      k_invalid,
      k_valid,
    };
    
    struct sockaddr_in6               addr_;
    struct timespec                   next_avail_time_;
    int                               outstanding_requests_;
    std::list<std::unique_ptr<sock>>  socks_;
  };
  
  void ep_exit(endpoint* ep)
  {
    --ep->outstanding_requests_;
    ++cur_avail_requests_;
    if (cur_avail_requests_ == total_possible_requests_)
      zero_outstanding_requests_cond_.notify_all();
    connections_possible_cond_.notify_one();
  }
  
  // sorted by preference (for example, known network latency
  // in reaching the ip address).
  std::multimap<int, std::unique_ptr<endpoint>> endpoints_;

};

