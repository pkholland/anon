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
#include "big_id_crypto.h"
#include <queue>
#include <memory>

class endpoint_cluster : public std::enable_shared_from_this<endpoint_cluster>
{
public:
  static std::shared_ptr<endpoint_cluster>
  create(const char *host, int port,
         bool do_tls = false,
         const tls_context *tls_ctx = 0,
         int max_conn_per_ep = 40,
         int lookup_frequency_in_seconds = 20)
  {
    return std::make_shared<endpoint_cluster>(host, port, do_tls, tls_ctx, max_conn_per_ep, lookup_frequency_in_seconds);
  }

  endpoint_cluster(const char *host, int port,
                   bool do_tls,
                   const tls_context *tls_ctx,
                   int max_conn_per_ep,
                   int lookup_frequency_in_seconds);

  void set_max_io_block_time(int max_io_block_time)
  {
    max_io_block_time_ = max_io_block_time;
  }

  void disable_retries()
  {
    retries_enabled_ = false;
  }

  void with_connected_pipe(const std::function<bool(const pipe_t *pipe)> &f)
  {
    if (retries_enabled_)
    {
      auto sleepMs = 50;
      auto slp = 0;
      while (true)
      {
        try
        {
          do_with_connected_pipe(f);
          return;
        }
        catch (const fiber_io_error &e)
        {
          delete_cached_endpoints();
          if (sleepMs > 30 * 1000)
            throw;
          auto rid = small_rand_id();
          auto ri = *(unsigned int *)&rid.m_buf[0];
          slp = sleepMs * 3 / 4 + (ri % (sleepMs / 2));
#ifdef ANON_LOG_DNS_LOOKUP
          anon_log("with_connected_pipe hit exception, what() = " << e.what() << ", sleeping for " << slp / 1000.0 << " seconds before trying again");
#endif
        }
        catch (const fiber_io_timeout_error &e)
        {
#ifdef ANON_LOG_DNS_LOOKUP
          anon_log("with_connected_pipe hit fiber_io_timeout_error, what() = " << e.what());
#endif
          sleepMs = 0;
        }
        if (sleepMs > 0)
        {
          fiber::msleep(slp);
          sleepMs *= 2;
        }
        else
          sleepMs = 50;
      }
    }
    else
    {
      cache_cleaner cc(this);
      do_with_connected_pipe(f);
      cc.complete();
    }
  }

  // each 'endpoint' is a single ip address
  // and a collection of 1 or more (up to max_conn_per_ep_)
  // open, connected sockets to that endpoint
  struct endpoint
  {
    endpoint(const struct sockaddr_in6 &addr)
        : addr_(addr),
          outstanding_requests_(0),
          last_lookup_time_(cur_time())
    {
    }

    struct sock
    {
      sock(std::unique_ptr<pipe_t> &&pipe)
          : pipe_(std::move(pipe))
      {
      }
      std::unique_ptr<pipe_t> pipe_;
      timespec idle_start_time;
    };

    struct sockaddr_in6 addr_;
    std::queue<std::shared_ptr<sock>> socks_;
    int outstanding_requests_;
    fiber_mutex mtx_;
    fiber_cond cond_;
    struct timespec last_lookup_time_;
  };

private:
  void do_with_connected_pipe(const std::function<bool(const pipe_t *pipe)> &f);
  void update_endpoints();

public:
  void erase(const std::shared_ptr<endpoint> &ep);
  void delete_cached_endpoints();

private:
  struct cache_cleaner
  {
    endpoint_cluster *epc;
    bool clean;
    cache_cleaner(endpoint_cluster *epc)
        : epc(epc),
          clean(true)
    {
    }

    ~cache_cleaner()
    {
      if (clean)
        epc->delete_cached_endpoints();
    }

    void complete()
    {
      clean = false;
    }
  };

  void erase_if_empty(const std::shared_ptr<endpoint> &ep);

  std::string host_;
  int port_;
  bool do_tls_;
  const tls_context *tls_ctx_;
  int max_conn_per_ep_;
  int lookup_frequency_in_seconds_;

  std::vector<std::shared_ptr<endpoint>> endpoints_;
  bool looking_up_endpoints_;
  bool retries_enabled_;
  fiber_mutex mtx_;
  fiber_cond cond_;
  struct timespec last_lookup_time_;
  int round_robin_index_;
  std::unique_ptr<fiber_io_error> lookup_err_;
  int max_io_block_time_;

  enum
  {
    k_default_io_block_time = 30,
    k_max_idle_time = 25
  };
};
