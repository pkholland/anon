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

#include <epc.h>
#include <netdb.h>
#include "dns_lookup.h"

endpoint_cluster* endpoint_cluster::first_epc_;
namespace {
  fiber_mutex mtx;
}

endpoint_cluster::endpoint_cluster(const char *host, int port,
                                   bool do_tls,
                                   const tls_context *tls_ctx,
                                   int max_conn_per_ep,
                                   int lookup_frequency_in_seconds)
    : host_(host),
      port_(port),
      do_tls_(do_tls),
      tls_ctx_(tls_ctx),
      max_conn_per_ep_(max_conn_per_ep),
      lookup_frequency_in_seconds_(lookup_frequency_in_seconds),
      last_lookup_time_((struct timespec){}),
      round_robin_index_(0),
      looking_up_endpoints_(false),
      max_io_block_time_(k_default_io_block_time),
      retries_enabled_(true),
      non_blocking_(true)
{
  fiber_lock l(mtx);
  if (first_epc_)
    first_epc_->prev_epc_ = this;
  next_epc_ = first_epc_;
  prev_epc_ = 0;
  first_epc_ = this;
}

endpoint_cluster::~endpoint_cluster()
{
  fiber_lock l(mtx);
  if (next_epc_)
    next_epc_->prev_epc_ = prev_epc_;
  if (prev_epc_)
    prev_epc_->next_epc_ = next_epc_;
  else
    first_epc_ = next_epc_;
}

void endpoint_cluster::erase_all()
{
  fiber_lock l(mtx);
  auto ep = first_epc_;
  while (ep) {
    ep->erase_all_endpoints();
    ep = ep->next_epc_;
  }
}

void endpoint_cluster::update_endpoints()
{
  auto addrs = dns_lookup::get_addrinfo(host_.c_str(), port_);

  fiber_lock l(mtx_);
  if (addrs.first != 0 || addrs.second.size() == 0)
  {
    // if there are no current endpoints that are already known
    // for this host name, then this error object will be thrown
    // in the fiber has initiated this call to update_endpoints.
    // In that case, the fact that it is an instance of fiber_io_error
    // will cause the exponential backoff logic to kick in and
    // re-attempt the operation - optimistically hoping that whatever
    // is causing the error will resolve itself
    lookup_err_ = std::unique_ptr<fiber_io_error>(new fiber_io_error(Log::fmt(
        [&](std::ostream &msg) { msg << "dns lookup failed for: " << host_ << ", error: " << (addrs.first < 0 ? gai_strerror(addrs.first) : error_string(addrs.first)); })));
  }
  else
  {
    // build a map which is the union of all currently
    // known endpoints plus the ones that were just
    // returned in the call to get_adddrinfo.
    auto now = cur_time();
    std::map<sockaddr_in6, std::shared_ptr<endpoint>> endpoints;
    for (auto &ep : endpoints_)
      endpoints.insert(std::make_pair(ep->addr_, ep));
    for (auto &addr : addrs.second)
    {
      auto it = endpoints.find(addr);
      if (it == endpoints.end())
        endpoints.insert(std::make_pair(addr, std::make_shared<endpoint>(addr)));
      else
        it->second->last_lookup_time_ = now;
    }

    // we use a simple policy rule that we are willing to
    // keep attempting to use any ip address that was returned
    // by getaddrinfo up to 10 times whatever the
    // dns lookup_frequency_in_seconds_ is.  If any cached
    // open sockets become unusable they will be deleted.
    // If new connection attempts fail to one of these
    // "slighly old" endpoints we will delete that endpoint
    // from our list.
    auto oldest = now - lookup_frequency_in_seconds_ * 10;
    for (auto it = endpoints.begin(); it != endpoints.end();)
    {
      if (it->second->last_lookup_time_ < oldest)
      {
#ifdef ANON_LOG_DNS_LOOKUP
        anon_log("aging out endpoint " << it->second->addr_ << ", because it was " << to_seconds(now - it->second->last_lookup_time_) << " seconds old");
#endif
        it = endpoints.erase(it);
      }
      else
        ++it;
    }
    endpoints_.resize(endpoints.size());
    int indx = 0;
#ifdef ANON_LOG_DNS_LOOKUP
    anon_log("host: " << host_);
#endif
    for (auto &p : endpoints)
    {
#ifdef ANON_LOG_DNS_LOOKUP
      auto age = to_seconds(now - p.second->last_lookup_time_);
      if (age < 0.0)
        age = 0.0;
      anon_log(" using " << p.second->addr_ << ", lookup age: " << age << " seconds");
#endif
      endpoints_[indx++] = p.second;
    }
  }

  last_lookup_time_ = cur_time();
  looking_up_endpoints_ = false;
  cond_.notify_all();
}

void endpoint_cluster::erase_all_endpoints()
{
  fiber_lock l(mtx_);
  endpoints_.resize(0);
  lookup_err_ = std::unique_ptr<fiber_io_error>(new fiber_io_error("erase_all_endpoints"));
}

// This is called either by erase_if_empty ep has only one open
// socket (erase_if_empty is called if there is a problem with
// that socket) - or if we get a connection error trying to
// open a new socket for this endpoint.  We delete the endpoint
// from our list to force dns to run again next time we try
// to get a functional pipe
void endpoint_cluster::erase(const std::shared_ptr<endpoint> &ep)
{
  fiber_lock l(mtx_);
  auto it = endpoints_.begin();
  while (it != endpoints_.end())
  {
    if (*it == ep)
    {
#ifdef ANON_LOG_DNS_LOOKUP
      anon_log("endpoint_cluster::erase emptying");
#endif
      endpoints_.erase(it);
      return;
    }
    it++;
  }
#ifdef ANON_LOG_DNS_LOOKUP
  anon_log("failed to erase endpoint " << ep->addr_);
#endif
}

// This is called when there is some error that has occurred
// on a socket related to this endpoint.  If it turns out that
// that socket was the only one in existence for this endpoint
// then remove the matching entry in the endpoints_ vector.
// This causes that endpoint to get deleted (when the other
// shared pointers to it are destructed) and means that the
// endpoint_cluster itself will not attempt to use this endpoint
// again without first going back through dns resolution.
// This cleans up cases where the ip address itself is no
// longer good and dns lookup is correctly no longer reporting
// that ip address.  We have a policy of continuing to attempt
// to use that ip address for some period after dns lookup has
// returned it.
void endpoint_cluster::erase_if_empty(const std::shared_ptr<endpoint> &ep)
{
  {
    fiber_lock l(ep->mtx_);
    if (ep->socks_.size() != 0 || ep->outstanding_requests_ != 1)
    {
#ifdef ANON_LOG_DNS_LOOKUP
      anon_log("endpoint_cluster::erase_if_empty, not emptying, ep->socks_.size(): " << ep->socks_.size() << ", ep->outstanding_requests_: " << ep->outstanding_requests_);
#endif
      return;
    }
  }

  erase(ep);
}

void endpoint_cluster::delete_cached_endpoints()
{
  fiber_lock l(mtx_);
  endpoints_.resize(0);
}

namespace
{

class cleanup
{
public:
  cleanup(const std::weak_ptr<endpoint_cluster::endpoint> &wep,
          const std::shared_ptr<endpoint_cluster::endpoint::sock> &sock,
          const std::weak_ptr<endpoint_cluster>& wcp)
      : wep(wep),
        sock(sock),
        cache(false),
        exception_thrown(true),
        wcp(wcp)
#ifdef ANON_LOG_DNS_LOOKUP
        ,addr(wep.lock()->addr_)
#endif
  {
  }

  ~cleanup()
  {
    auto ep = wep.lock();
    if (ep)
    {
      fiber_lock l(ep->mtx_);
      --ep->outstanding_requests_;
      if (exception_thrown) {
#ifdef ANON_LOG_DNS_LOOKUP
        anon_log("cleanup::~cleanup - exception thrown");
#endif
        auto cp = wcp.lock();
        if (cp) {
#ifdef ANON_LOG_DNS_LOOKUP
        anon_log("cleanup::~cleanup - exception thrown, erasing " << ep->addr_);
#endif
          cp->erase(ep);
        }
      }
      if (cache)
      {
        sock->idle_start_time = cur_time();
        ep->socks_.push(sock);
      }
      ep->error_ |= exception_thrown;
      ep->cond_.notify_all();
    }
    else
    {
#ifdef ANON_LOG_DNS_LOOKUP
      anon_log("epc appears that endpoint for " << addr << " was deleted prior to callback returning");
#endif
    }
  }

  std::weak_ptr<endpoint_cluster::endpoint> wep;
  std::shared_ptr<endpoint_cluster::endpoint::sock> sock;
  std::weak_ptr<endpoint_cluster> wcp;
  bool cache;
  bool exception_thrown;

#ifdef ANON_LOG_DNS_LOOKUP
  const struct sockaddr_in6 addr;
#endif
};

class eraser
{
public:
  eraser(endpoint_cluster *epc,
         const std::shared_ptr<endpoint_cluster::endpoint> &ep)
      : epc(epc),
        ep(ep),
        success(false)
  {
  }

  ~eraser()
  {
    if (!success)
      epc->erase(ep);
  }

  endpoint_cluster *epc;
  std::shared_ptr<endpoint_cluster::endpoint> ep;
  bool success;
};

} // namespace

void endpoint_cluster::do_with_connected_pipe(const std::function<bool(const pipe_t *pipe)> &f)
{
  // if there are currently no available endpoints, or if it has been too long since we have
  // last looked up endpoints, then restart the lookup process.  If there are no endpoints
  // then wait until the lookup is complete, otherwise continue to use the endpoints we
  // already know about and let the lookup complete asynchronously.  If there was an error
  // attempting the lookup throw that error if there are no current endpoints, otherwise
  // ignore the error
  std::shared_ptr<endpoint> ep;
  std::weak_ptr<endpoint> wep;
  {
    fiber_lock l(mtx_);
    if (endpoints_.size() == 0 || to_seconds(cur_time() - last_lookup_time_) > lookup_frequency_in_seconds_)
    {
      if (!looking_up_endpoints_)
      {
        looking_up_endpoints_ = true;
        lookup_err_.reset();
        std::weak_ptr<endpoint_cluster> wp = shared_from_this();
        auto stack_size = 16 * 1024;
        fiber::run_in_fiber(
            [wp] {
              auto ths = wp.lock();
              if (ths)
              {
                ths->update_endpoints();
              }
            },
            stack_size, "epc, update_endpoints");
      }
      while (endpoints_.size() == 0)
      {
        cond_.wait(l);
        if (lookup_err_) {
          #if defined(ANON_LOG_ALL_THROWS)
          anon_log(lookup_err_->what());
          #endif
          throw *lookup_err_;
        }
      }
    }
    ep = endpoints_[round_robin_index_++ % endpoints_.size()];
    wep = ep;
  }

  std::shared_ptr<endpoint::sock> sock;
  {
    fiber_lock l(ep->mtx_);

    while (ep->outstanding_requests_ >= max_conn_per_ep_)
    {
#ifdef ANON_LOG_DNS_LOOKUP
      anon_log("waiting to use endpoint " << ep->addr_ << " because it has " << ep->outstanding_requests_ << " outstanding requests");
#endif
      ep->cond_.wait(l);
    }
    if (ep->error_)
    {
#ifdef ANON_LOG_DNS_LOOKUP
      anon_log("endpoint for " << ep->addr_ << " had previous error (and has been removed from endpoint list) - trying again");
#endif
      l.unlock();
      ep.reset();
      do_with_connected_pipe(f);
      return;
    }
    ++ep->outstanding_requests_;

    while (!sock && ep->socks_.size() != 0)
    {
      auto s = ep->socks_.front();
      ep->socks_.pop();
      if (cur_time() < s->idle_start_time + k_max_idle_time)
        sock = s;
#ifdef ANON_LOG_DNS_LOOKUP
      else
        anon_log("releasing socket (fd=" << s->pipe_->get_fd() << ", from " << ep->addr_ << ") because it has been idle for " << cur_time() - s->idle_start_time << " seconds");
#endif
    }

    if (sock)
    {
#ifdef ANON_LOG_DNS_LOOKUP
      anon_log("epc reused connection (fd=" << sock->pipe_->get_fd() << ", idle_time=" << cur_time() - sock->idle_start_time << ") to " << ep->addr_);
#endif
    }
    else
    {
      l.unlock();
      eraser era(this, ep);
      auto conn = tcp_client::connect((struct sockaddr *)&ep->addr_, ep->addr_.sin6_family == AF_INET6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in), non_blocking_);
      era.success = true;
      if (conn.first != 0)
      {
        erase(ep);
        //anon_log("tcp connect failed for " << ep->addr_ << " (" << host_ << "), error: " << error_string(conn.first) << ", lookup age: " << to_seconds(cur_time() - last_lookup_time_) << " seconds");
        anon_throw(fiber_io_error, "tcp connect failed for " << ep->addr_ << "(" << host_ << "), error: " << error_string(conn.first));
      }
      conn.second->limit_io_block_time(max_io_block_time_);

      std::unique_ptr<pipe_t> pipe;
      if (do_tls_)
        pipe = std::unique_ptr<pipe_t>(new tls_pipe(std::move(conn.second),
                                                    true, // client (not server)
                                                    verify_peer_,
                                                    do_SNI_,
                                                    host_.c_str(),
                                                    *tls_ctx_));
      else
        pipe = std::unique_ptr<pipe_t>(conn.second.release());
      sock = std::shared_ptr<endpoint::sock>(new endpoint::sock(std::move(pipe)));
#ifdef ANON_LOG_DNS_LOOKUP
      sockaddr_in6 local_addr;
      auto local_addr_len = (socklen_t)sizeof(local_addr);
      getsockname(sock->pipe_->get_fd(), (struct sockaddr*)&local_addr, &local_addr_len);
      anon_log("epc established new connection (fd=" << sock->pipe_->get_fd() << ") to " << ep->addr_ << " from " << local_addr << ", non_blocking_: " << (non_blocking_ ? "true" : "false"));
#endif
    }
  }

  // we let go of the endpoint itself and only hold
  // the weak pointer to it across the call.  This
  // lets it timeout and get deleted more smoothly.
  cleanup cu(wep, sock, shared_from_this());
  ep.reset();
  cu.cache = f(sock->pipe_.get());
  cu.exception_thrown = false;
}
