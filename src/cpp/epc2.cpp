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

#include <epc2.h>

endpoint_cluster::endpoint_cluster(const char* host, int port,
        bool do_tls,
        const tls_context *ctx,
        int max_conn_per_ep,
        int lookup_frequency_in_seconds)
  : host_(host),
    port_(port),
    do_tls_(do_tls),
    ctx_(ctx),
    max_conn_per_ep_(max_conn_per_ep),
    lookup_frequency_in_seconds_(lookup_frequency_in_seconds),
    last_lookup_time_((struct timespec){}),
    round_robin_index_(0)
{
}

// runs with mtx_ locked, 
void endpoint_cluster::update_endpoints()
{

}

void endpoint_cluster::do_with_connected_pipe(const std::function<void(const pipe_t *pipe)> &f)
{
  std::shared_ptr<endpoint> ep;
  {
    fiber_lock  l(mtx_);
    if (endpoints_.size() == 0 || to_seconds(cur_time() - last_lookup_time_) > lookup_frequency_in_seconds_)
      update_endpoints();
    ep = endpoints_[round_robin_index_++ % endpoints_.size()];
  }

  std::shared_ptr<endpoint::sock> sock;
  {
    fiber_lock l(ep->mtx_);
    while (ep->socks_.size() + ep->outstanding_requests_ >= max_conn_per_ep_)
      ep->cond_.wait(l);
    ++ep->outstanding_requests_;
    if (ep->socks_.size() != 0)
    {
      sock = ep->socks_.front();
      ep->socks_.pop();
    }
    else
    {
      l.unlock();
      auto conn = tcp_client::connect((struct sockaddr *)&ep->addr_, ep->addr_.sin6_family == AF_INET6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in));
      switch (conn.first) {
        case 0: {
            std::unique_ptr<pipe_t> pipe;
            if (do_tls_)
              pipe = std::unique_ptr<pipe_t>(new tls_pipe(std::move(conn.second),
                                                          true, // client (not server)
                                                          true, // verify_peer
                                                          true, // doSNI
                                                          host_.c_str(),
                                                          *ctx_));
            else
              pipe = std::unique_ptr<pipe_t>(conn.second.release());
            sock = std::shared_ptr<endpoint::sock>(new endpoint::sock(std::move(pipe)));
          }  break;
        case ENETUNREACH:
        case ECONNREFUSED:
        case ETIMEDOUT:
          break;
        default: // some unknown problem
          break;
      }
    }
  }
}
