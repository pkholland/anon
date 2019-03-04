/*
 Copyright (c) 2019 Anon authors, see AUTHORS file.
 
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
#include <queue>

class endpoint_cluster : public std::enable_shared_from_this<endpoint_cluster>
{
public:
  static std::shared_ptr<endpoint_cluster>
  create(const char* host, int port,
        bool do_tls = false,
        const tls_context *ctx = 0,
        int max_conn_per_ep = 20,
        int lookup_frequency_in_seconds = 1)
  {
    return std::make_shared<endpoint_cluster>(host, port, do_tls, ctx, max_conn_per_ep, lookup_frequency_in_seconds);
  }

  endpoint_cluster(const char* host, int port,
        bool do_tls,
        const tls_context *ctx,
        int max_conn_per_ep,
        int lookup_frequency_in_seconds);


  void with_connected_pipe(const std::function<void(const pipe_t *pipe)> &f)
  {
    int sleepMs = 50;
    while (true)
    {
      try
      {
        do_with_connected_pipe(f);
        return;
      }
      catch (const backoff_error &)
      {
        fiber::msleep(sleepMs);
        sleepMs *= 2;
        if (sleepMs > 30 * 1000)
          throw;
      }
    }
  }

private:
  void do_with_connected_pipe(const std::function<void(const pipe_t *pipe)> &f);
  void update_endpoints();

  // each 'endpoint' is a single ip address
  // and a collection of 1 or more (up to max_conn_per_ep_)
  // open, connected sockets to that endpoint
  struct endpoint {

    endpoint(const struct sockaddr_in6 &addr)
      : addr_(addr),
        outstanding_requests_(0),
        last_lookup_time_(cur_time())
    {}

    struct sock
    {
      sock(std::unique_ptr<pipe_t> &&pipe)
        : pipe_(std::move(pipe)),
          first_use_(true)
      {}

      std::unique_ptr<pipe_t> pipe_;
      bool first_use_;
    };

    struct sockaddr_in6 addr_;
    std::queue<std::shared_ptr<sock>> socks_;
    int outstanding_requests_;
    fiber_mutex mtx_;
    fiber_cond cond_;
    struct timespec last_lookup_time_;

  };

  struct backoff_error : public fiber_io_error
  {
  };

  std::string host_;
  int port_;
  bool do_tls_;
  const tls_context* ctx_;
  int max_conn_per_ep_;
  int lookup_frequency_in_seconds_;

  std::vector<std::shared_ptr<endpoint>>  endpoints_;
  fiber_mutex mtx_;
  struct timespec last_lookup_time_;
  int round_robin_index_;

};