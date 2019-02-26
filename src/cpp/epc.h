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

#pragma once

#include "tcp_client.h"
#include "tls_pipe.h"
#include "tls_context.h"
#include <list>

class endpoint_cluster : public std::enable_shared_from_this<endpoint_cluster>
{
public:
  // given a function 'lookup', construct an endpoint_cluster
  // whose with_connected_pipe method will connect to and use
  // the ip address(es) returned by 'lookup'
  //
  //  lookup must have the following signature:
  //
  //    std::pair<int err_code, std::vector<std::pair<int preference, sockaddr_in6>>> lookup(void);
  //
  //  The idea is that lookup will be called and is expected to either return
  //  an error condition (err_code != 0) or return a vector of one or more
  //  ip addresses, each paired with a "preference" value.  The preference value
  //  specifies the preferred order of the returned ip addresses with lower values
  //  being more preferred than higher values.  An example of how an implimentation
  //  might use this is if it were to measure the ping time between this host machine
  //  and each of the addresses being returned, this ping time might be a suitable
  //  value for preference, since it could indicate addresses that are faster to
  //  communicate with.  While the ip address is represented as a sockaddr_in6,
  //  it can contain either a sockaddr_in or sockaddr_in6 (by specifying the
  //  "family" field correctly).
  //
  //  This lookup function is always called on a fiber.  It will be called immediately
  //  upon constructing this object and then periodically every lookup_frequency_in_seconds
  //  seconds thereafter.  If lookup_frequency_in_seconds <= 0 then it will only be called
  //  once when this endpoint_cluster is constructed.
  //
  //  max_conn_per_ep is the maximum number of sockets this endpoint_cluster will
  //  attempt to create and connect to each of the ip addresses returned by lookup.
  //  ip addresses with a higher preference value (so less preferred) will only be
  //  used if the all more-preferred addresses are unavailable.  An address can be
  //  unavailable if there are already max_conn_per_ep calls to with_connected_pipe
  //  executing in other fibers at the time that a new call to with_connected_pipe
  //  is made.  It can also happen if certain "back off" strategies are currently in
  //  effect for that ip address at the time that with_connected_pipe is called.
  endpoint_cluster(const std::function<std::pair<int, std::vector<std::pair<int, sockaddr_in6>>>()> &lookup,
                   bool do_tls = false,
                   const char *host_name_for_tls = "",
                   const tls_context *ctx = 0,
                   int max_conn_per_ep = 20,
                   int lookup_frequency_in_seconds = 120)
      : lookup_(lookup),
        shutting_down_(false),
        do_tls_(do_tls),
        host_name_for_tls_(host_name_for_tls),
        tls_ctx_(ctx),
        max_conn_per_ep_(max_conn_per_ep),
        round_robin_index_(0),
        cur_avail_requests_(0),
        total_possible_requests_(0),
        lookup_error_(0),
        total_fibers_(0),
        lookup_frequency_seconds_(lookup_frequency_in_seconds),
        update_running_(false)
  {
    // run as a task (right now) instead of directly starting
    // the fiber so the destructor (actually, do_shutdown) can
    // reliably wait for proper exit.
    update_task_ = io_dispatch::schedule_task(
        [this] {
          fiber::run_in_fiber([this] { update_endpoints(); });
        },
        cur_time());

    fiber_pipe::register_idle_socket_sweep(this, [this] { idle_socket_sweep(); });
  }

  //  call the given 'f' with a pipe containing a socket that is
  //  connected to one of the ip address returned by the 'lookup'
  //  parameter given to the constructor of this endpoint_cluster.
  //
  //  The signature of 'f' must be:
  //
  //    void f(const pipe_t* pipe)
  //
  //  Note that 'f' can be called more than once, or even not at
  //  all.  When called 'pipe' will contain a socket was at some point
  //  in the past connected to the ip address.  But the other end of
  //  this socket may be closed or in some other invalid state by
  //  the time 'f' attempts to use it.  This normaly causes c++
  //  exceptions to be thrown from within the read and write functions
  //  for the pipe.  Those are normally caught by this code and trigger
  //  attempts to reconnect to the endpoint, followed by calling 'f'
  //  again with a new pipe.
  //  Because of this behavior it is generally a good idea to perform
  //  your significant calculations to compute what you wish to write
  //  to the pipe prior to calling with_connected_pipe, and then mostly
  //  perform the write itself (perhaps followed by read) inside of 'f'.
  void with_connected_pipe(const std::function<void(const pipe_t *pipe)> &f)
  {
    fiber_counter fc(this);

    while (true)
    {
      try
      {
        do_with_connected_pipe(f);
        return;
      }
      catch (const backoff_error &)
      { // retry these
      }
    }
  }

  ~endpoint_cluster()
  {
    do_shutdown();
  }

private:
  fiber_mutex mtx_;
  fiber_cond zero_fibers_cond_;
  fiber_cond connections_possible_cond_;
  fiber_cond update_cond_;
  int max_conn_per_ep_;
  unsigned int round_robin_index_;
  int cur_avail_requests_;
  int total_possible_requests_;
  int lookup_error_;
  int total_fibers_;
  int lookup_frequency_seconds_;
  io_dispatch::scheduled_task update_task_;
  std::string host_name_for_tls_;
  const tls_context *tls_ctx_;
  bool shutting_down_;
  bool do_tls_;
  bool update_running_;

  std::multimap<struct timespec, io_dispatch::scheduled_task> io_retry_tasks_;

  void do_shutdown()
  {
    fiber_pipe::remove_idle_socket_sweep(this);

    // so we don't kick off a new call to update_endpoints
    io_dispatch::remove_task(update_task_);

    fiber_lock lock(mtx_);

    // so we don't attempt any new background work
    shutting_down_ = true;

    while (update_running_)
      update_cond_.wait(lock);

    // kill all outstanding io retries
    // (we don't bother deleting the io_retry_tasks_ themselves, since
    // they will be deleted when this is destructed. here we are just
    // killing the tasks themselves so we don't get called when they expire).
    for (auto it = io_retry_tasks_.begin(); it != io_retry_tasks_.end(); it++)
      io_dispatch::remove_task(it->second);

    // wake all remaining, stalled/backed-off fibers.
    // they will all throw exceptions when they come out
    // of their wait call and see shutting_down_ == true
    connections_possible_cond_.notify_all();

    // now wait for all in-flight calls to with_connect_pipe
    // to get to the end of that function
    while (total_fibers_ > 0)
      zero_fibers_cond_.wait(lock);
  }

  void idle_socket_sweep();

  struct backoff_error
  {
  };

  struct fiber_counter
  {
    fiber_counter(endpoint_cluster *epc)
        : epc_(epc)
    {
      fiber_lock lock(epc_->mtx_);
      ++epc_->total_fibers_;
    }

    ~fiber_counter()
    {
      fiber_lock lock(epc_->mtx_);
      if (--epc_->total_fibers_ == 0)
        epc_->zero_fibers_cond_.notify_one();
    }

    endpoint_cluster *epc_;
  };
  friend struct fiber_counter;

  std::function<std::pair<int, std::vector<std::pair<int, sockaddr_in6>>>()> lookup_;

  // each 'endpoint' is a single ip address
  // and a collection of 1 or more (up to max_conn_per_ep_)
  // open, connected sockets to that endpoint
  struct endpoint
  {
    endpoint(const struct sockaddr_in6 &addr)
        : outstanding_requests_(0),
          is_detached_(false),
          backoff_exp_(0),
          success_count_(0)
    {
      size_t addrlen = (addr.sin6_family == AF_INET6) ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
      memcpy(&addr_, &addr, addrlen);
      next_avail_time_.tv_sec = std::numeric_limits<decltype(next_avail_time_.tv_sec)>::min();
      next_avail_time_.tv_nsec = 0;
    }

    ~endpoint()
    {
      // fancy "tell h2 to shutdown, then wait until fiber exits"
    }

    struct sock
    {
      sock(std::unique_ptr<pipe_t> &&pipe)
          : pipe_(std::move(pipe)),
            in_use_(true)
      {
      }

      std::unique_ptr<pipe_t> pipe_;
      bool in_use_;
      struct timespec last_used_time_;
    };

    enum
    {
      k_invalid,
      k_valid,
    };

    struct sockaddr_in6 addr_;
    struct timespec next_avail_time_;
    int outstanding_requests_;
    int backoff_exp_;
    int success_count_;
    bool is_detached_;
    std::list<std::unique_ptr<sock>> socks_;
  };

  void ep_exit(endpoint *ep)
  {
    --ep->outstanding_requests_;

    // a "detatched" endpoint is one which was at one point
    // being reported as a valid ip_addr by the lookup mechanism,
    // but then at some later point is no longer reported as
    // a valid ip_addr.  When this transition occurs while
    // one or more fibers are still using this endpoint, it
    // becomes "detached".  We no longer attempt new requests
    // on detached endpoints, and just wait for the existing
    // requests to complete before deleting the endpoint.
    if (ep->is_detached_ && ep->outstanding_requests_ == 0)
    {
      for (auto ep_it = endpoints_.begin(); ep_it != endpoints_.end(); ++ep_it)
      {
        if (ep_it->second.get() == ep)
        {
          endpoints_.erase(ep_it);
          break;
        }
      }
    }
    else
    {
      ++cur_avail_requests_;
      connections_possible_cond_.notify_one();
    }
  }

  void update_endpoints();
  void backoff(endpoint *ep, const struct timespec &start_time, int explicit_seconds = 0);
  void do_with_connected_pipe(const std::function<void(const pipe_t *pipe)> &f);

  // sorted by preference (for example, known network latency
  // in reaching the ip address).
  std::multimap<int, std::unique_ptr<endpoint>> endpoints_;
};
