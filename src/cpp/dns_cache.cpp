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

#include "dns_cache.h"
#include "tcp_utils.h"
#include "lock_checker.h"
#include "time_utils.h"
#include <limits>
#include <netdb.h>
#include <pthread.h>
#include <limits.h>

namespace dns_cache
{

static io_dispatch::scheduled_task next_sweep;
static void sweep_old_cache_entries();

void initialize()
{
  sweep_old_cache_entries();
}

void terminate()
{
  io_dispatch::remove_task(next_sweep);
}

// a time much later than now...
static struct timespec forever = {std::numeric_limits<time_t>::max(), 1000000000 - 1};

class dns_entry
{
public:
  dns_entry()
      : last_(0),
        state_(k_init)
  {
  }

  bool call(const char *host, int port, const std::function<void(int err_code, const struct sockaddr *addr, socklen_t addrlen)> &dnsc, size_t stack_size, struct sockaddr_in6 &addr)
  {
    switch (state_)
    {
    case k_init:
      initiate_lookup(host, port, dnsc, stack_size);
      return false;
    case k_in_progress:
      pending_callers_.push_back(pending_call(host, port, dnsc, stack_size));
      return false;
    case k_resolved:
      return call_from_cache(host, port, dnsc, stack_size, addr);
    case k_failed_resolve:
      do_error("invalid call to dns::lookup_and_run while dns cache entry for \"" << host << "\" is in a failed state");
    default:
      do_error("unknown dns_entry::state_ " << state_);
    }
    return false;
  }

private:
  friend void dns_cache::sweep_old_cache_entries();

  // tell dns callers about failure conditions
  // and then delete the callback object
  static void inform_in_fiber(const std::function<void(int err_code, const struct sockaddr *addr, socklen_t addrlen)> &dnsc, int err)
  {
    fiber::run_in_fiber(
        [dnsc, err] {
          dnsc(err, 0, 0);
        },
        fiber::k_default_stack_size, "dns_cache failure");
  }

  void inform_in_fiber(int err)
  {
    while (!pending_callers_.empty())
    {
      auto pc = pending_callers_.back();
      inform_in_fiber(pc.dnsc_, err);
      pending_callers_.pop_back();
    }
  }

  static void resolve_complete(union sigval sv);
  void initiate_lookup(const char *host, int port, const std::function<void(int err_code, const struct sockaddr *addr, socklen_t addrlen)> &dnsc, size_t stack_size);
  bool call_from_cache(const char *host, int port, const std::function<void(int err_code, const struct sockaddr *addr, socklen_t addrlen)> &dnsc, size_t stack_size, struct sockaddr_in6 &addr);

  // structure used to hold state data
  // during the getaddrinfo_a lookup
  struct notify_complete
  {
    notify_complete(const char *host, const std::function<void(int err_code, const struct sockaddr *addr, socklen_t addrlen)> &dnsc, size_t stack_size, dns_entry *entry)
        : host_(host),
          dnsc_(dnsc),
          stack_size_(stack_size),
          entry_(entry)
    {
    }

    std::string host_;
    const std::function<void(int err_code, const struct sockaddr *addr, socklen_t addrlen)> dnsc_;
    size_t stack_size_;
    dns_entry *entry_;
    struct gaicb cb_;

    struct addrinfo hints_;
    struct sigevent se_;
    pthread_attr_t ptattr_;
    char portString_[8];
  };

  struct addr
  {
    addr(const struct sockaddr *addr)
        : valid_(true)
    {
      size_t addrlen = (addr->sa_family == AF_INET6) ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
      memcpy(&addr_, addr, addrlen);
      next_avail_time_.tv_sec = std::numeric_limits<time_t>::min();
      next_avail_time_.tv_nsec = 0;
    }

    struct sockaddr_in6 addr_;
    struct timespec next_avail_time_;
    bool valid_;
  };

  struct pending_call
  {
    pending_call(const char *host, int port, const std::function<void(int err_code, const struct sockaddr *addr, socklen_t addrlen)> &dnsc, size_t stack_size)
        : host_(host),
          port_(port),
          dnsc_(dnsc),
          stack_size_(stack_size)
    {
    }

    std::string host_;
    int port_;
    std::function<void(int err_code, const struct sockaddr *addr, socklen_t addrlen)> dnsc_;
    size_t stack_size_;
  };

  enum
  {
    k_init,
    k_in_progress,
    k_resolved,
    k_failed_resolve
  };

  std::vector<addr> addrs_;
  std::vector<pending_call> pending_callers_;
  int last_;
  int state_;
  struct timespec when_resolved_;
};

static std::mutex dns_map_mutex;
static std::map<std::string, dns_entry> dns_map;

////////////////////////////////////////////////////////////////

static void sweep_old_cache_entries()
{
  auto last_valid_time = cur_time() - cache_life_seconds;

  {
    anon::lock_guard<std::mutex> lock(dns_map_mutex);
    for (auto it = dns_map.begin(); it != dns_map.end();)
    {
      if (it->second.state_ == dns_entry::k_resolved && it->second.when_resolved_ < last_valid_time)
        dns_map.erase(it++);
      else
        it++;
    }
  }

  next_sweep = io_dispatch::schedule_task(sweep_old_cache_entries, cur_time() + cache_life_seconds / 2);
}

////////////////////////////////////////////////////////////////

// callback that getaddrinfo_a calls when
// async dns resolution completes
void dns_entry::resolve_complete(union sigval sv)
{
  anon::lock_guard<std::mutex> lock(dns_map_mutex);

  // we set this when we called getaddrinfo_a
  // so read it back here
  auto nc = (notify_complete *)sv.sival_ptr;

  // why were we called back?
  int ret = gai_error(&nc->cb_);
  if (ret == EAI_INPROGRESS)
  {

    anon_log_error("strange call to resolve_complete with gai_error returning EAI_INPROGRESS");
    return;
  }
  else if (ret != 0)
  {

#if defined(ANON_LOG_DNS_LOOKUP)
    anon_log_error("getaddrinfo_a completed with error: " << gai_strerror(ret));
#endif

    nc->entry_->state_ = k_failed_resolve;
    nc->entry_->inform_in_fiber(ret);
    auto e = dns_map.find(nc->host_);
    if (e != dns_map.end())
      dns_map.erase(e);
  }
  else
  {

    // dns lookup succeeded

#if defined(ANON_LOG_DNS_LOOKUP)
    int num_returns = 0;
    auto rslt = nc->cb_.ar_result;
    while (rslt)
    {
      ++num_returns;
      rslt = rslt->ai_next;
    }
    anon_log("dns lookup for \"" << nc->host_.c_str() << "\" returned " << num_returns << " result" << (num_returns > 1 ? "s:" : ":"));
    rslt = nc->cb_.ar_result;
    while (rslt)
    {
      anon_log("  " << *rslt->ai_addr);
      rslt = rslt->ai_next;
    }
#endif

    auto ths = nc->entry_;

    // take a snapshot of the addresses returned by the dns mechanism
    // and record when this snapshot was taken
    auto addinf = nc->cb_.ar_result;
    while (addinf)
    {
      ths->addrs_.push_back(addr(addinf->ai_addr));
      addinf = addinf->ai_next;
    }
    freeaddrinfo(nc->cb_.ar_result);
    ths->when_resolved_ = cur_time();
    ths->state_ = k_resolved;

    // run all of the pending functions, rotating through
    // the addresses (in case there is more than one)
    int i = 0;
    while (!ths->pending_callers_.empty())
    {

      auto pc = ths->pending_callers_.back();
      auto addr = ths->addrs_[i % ths->addrs_.size()].addr_;

      fiber::run_in_fiber(
          [pc, addr] {
            if (addr.sin6_family == AF_INET6)
              ((struct sockaddr_in6 *)&addr)->sin6_port = htons(pc.port_);
            else
              ((struct sockaddr_in *)&addr)->sin_port = htons(pc.port_);
            pc.dnsc_(0, (struct sockaddr *)&addr, addr.sin6_family == AF_INET6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in));
          },
          pc.stack_size_, "dns cache resolution complete");

      ths->pending_callers_.pop_back();
      ++i;
    }
  }
  pthread_attr_destroy(&nc->ptattr_);
  delete nc;
}

// use linux's getaddrinfo_a to do an async
// dns lookup
void dns_entry::initiate_lookup(const char *host, int port, const std::function<void(int err_code, const struct sockaddr *addr, socklen_t addrlen)> &dnsc, size_t stack_size)
{
  state_ = k_in_progress;

#if defined(ANON_LOG_DNS_LOOKUP)
  anon_log("starting dns lookup for \"" << host << "\", port " << port);
#endif

  // the info we will need when resolve_complete is called
  // also, while the man pages for getaddrinfo_a aren't
  // particularly clear on this, the pointers given to it
  // in the various data structures need to _stay valid_
  // until the callback function is called.  So we can't
  // put these data structures on the stack.  They are members
  // of the notify_complete object, which is kept around until
  // we are called back.
  auto nc = new notify_complete(host, dnsc, stack_size, this);

  // we don't need a very big stack for the
  // thread that libanl will use to call us
  // back on, so set the pthread stack size
  // small here
  int rslt = pthread_attr_init(&nc->ptattr_);
  if (rslt != 0)
  {
    anon_log_error("pthread_attr_init(&nc->ptattr_) failed with result: " << error_string(rslt));
    inform_in_fiber(dnsc, rslt);
    delete nc;
    dns_map.erase(dns_map.find(host));
    throw std::system_error(rslt, std::system_category());
  }
  auto sz = std::max(64 * 1024, PTHREAD_STACK_MIN);
  rslt = pthread_attr_setstacksize(&nc->ptattr_, sz);
  if (rslt != 0)
  {
    anon_log_error("pthread_attr_setstacksize(&nc->ptattr_, " << sz << ") failed with result: " << error_string(rslt));
    pthread_attr_destroy(&nc->ptattr_);
    inform_in_fiber(dnsc, rslt);
    delete nc;
    dns_map.erase(dns_map.find(host));
    throw std::system_error(rslt, std::system_category());
  }

  // gai takes the port parameter as a string
  sprintf(&nc->portString_[0], "%d", port);

  // the sorts of endpoints we are looking for
  memset(&nc->hints_, 0, sizeof(nc->hints_));
  nc->hints_.ai_family = AF_UNSPEC; // use IPv4 or IPv6, whichever
  nc->hints_.ai_socktype = SOCK_STREAM;

  // what we are trying to look up,
  // where to store the result...
  nc->cb_.ar_name = nc->host_.c_str();
  nc->cb_.ar_service = &nc->portString_[0];
  nc->cb_.ar_request = &nc->hints_;

  // this one is ok to put on the stack
  struct gaicb *cba = &nc->cb_;

  // how to notify us when the lookup completes
  memset(&nc->se_, 0, sizeof(nc->se_));
  nc->se_.sigev_notify = SIGEV_THREAD;
  nc->se_.sigev_value.sival_ptr = nc;
  nc->se_.sigev_notify_function = &dns_entry::resolve_complete;
  nc->se_.sigev_notify_attributes = &nc->ptattr_;

  // list this dnsc as one of the ones that will get called
  // once the resolution completes.
  pending_callers_.push_back(pending_call(host, port, dnsc, stack_size));

  // start the async getaddrinfo lookup
  int ret = getaddrinfo_a(GAI_NOWAIT, &cba, 1, &nc->se_);

  if (ret != 0)
  {
    anon_log_error("getaddrinfo_a(GAI_NOWAIT, &cba, 1, &se) failed with error: " << gai_strerror(ret));
    pthread_attr_destroy(&nc->ptattr_);
    inform_in_fiber(dnsc, ret);
    delete nc;
    dns_map.erase(dns_map.find(host));
    throw std::runtime_error(gai_strerror(ret));
  }
}

bool dns_entry::call_from_cache(const char *host, int port, const std::function<void(int err_code, const struct sockaddr *addr, socklen_t addrlen)> &dnsc, size_t stack_size, struct sockaddr_in6 &addr)
{
  auto now = cur_time();
  auto earliest = forever;

  // loop through looking for the next addr
  // that is valid and available.  If we can
  // find one we tell the caller to use that one.
  // If none are currently available, record the
  // earliest one that will become available, and
  // set a timer to wake up and try again then.
  int start_index = last_++;
  for (int i = 0; i < addrs_.size(); i++)
  {
    int index = (start_index + i) % addrs_.size();
    if (!addrs_[index].valid_)
      continue;
    if (addrs_[index].next_avail_time_ <= now)
    {
      addr = addrs_[index].addr_;
      return true;
    }
    else if (addrs_[index].next_avail_time_ < earliest)
      earliest = addrs_[index].next_avail_time_;
  }

  // none currently available, schedule a task
  // to try again at 'earliest'
  std::string host_copy = host;
  io_dispatch::schedule_task([host_copy, port, dnsc, stack_size] {
    lookup_and_run(host_copy.c_str(), port, dnsc, stack_size);
  },
                             earliest);

  return false;
}

////////////////////////////////////////////////////////

void lookup_and_run(const char *host, int port, const std::function<void(int err_code, const struct sockaddr *addr, socklen_t addrlen)> &dnsc, size_t stack_size)
{
  // we don't want to start a new fiber from this thread/fiber
  // while dns_map_mutex is locked (starting new fibers when
  // called from another fiber involves writing in to a fiber
  // pipe, and that can block, causing us to come back from
  // run_in_fiber on a different os thread).  So, if 'call'
  // wants to initiate an immediate call to dnsc, it does so
  // by filling out the 'addr' we supply and returning true.
  // We then unlock the mutex and call after it is unlocked.
  struct sockaddr_in6 addr;
  bool immediate;
  {
    anon::lock_guard<std::mutex> lock(dns_map_mutex);
    immediate = dns_map[host].call(host, port, dnsc, stack_size, addr);
  }
  if (immediate)
  {
    fiber::run_in_fiber(
        [port, dnsc, addr] {
          if (addr.sin6_family == AF_INET6)
            ((struct sockaddr_in6 *)&addr)->sin6_port = htons(port);
          else
            ((struct sockaddr_in *)&addr)->sin_port = htons(port);
          dnsc(0, (struct sockaddr *)&addr, addr.sin6_family == AF_INET6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in));
        },
        fiber::k_default_stack_size, "lookup_and_run immediate call");
  }
}

////////////////////////////////////////////////////////

int get_addrinfo(const char *host, int port, struct sockaddr_in6 *addr, socklen_t *addrlen)
{
  fiber_cond cond;
  fiber_mutex mtx;
  bool done = false;
  int ret = 0;

  // f is called in every case other than when
  // dns_entry::call_from_cache returns true.  In that
  // case dns_entry::call fills out addr before returning.
  auto dnsc = [&ret, &addr, &addrlen, &done, &mtx, &cond](int err_code, const struct sockaddr *_addr, socklen_t _addrlen) {
    ret = err_code;
    if (ret == 0)
    {
      memcpy(addr, _addr, _addrlen);
      *addrlen = _addrlen;
    }

    fiber_lock lock(mtx);
    done = true;
    cond.notify_all();
  };

  bool immediate;
  {
    anon::lock_guard<std::mutex> lock(dns_map_mutex);
    immediate = dns_map[host].call(host, port, dnsc, fiber::k_default_stack_size, *addr);
  }
  if (!immediate)
  {
    // wait for 'f' above to be called (once dns resolution is done)
    fiber_lock lock(mtx);
    while (!done)
      cond.wait(lock);
  }
  if (ret == 0)
  {
    if (addr->sin6_family == AF_INET6)
      ((struct sockaddr_in6 *)addr)->sin6_port = htons(port);
    else
      ((struct sockaddr_in *)addr)->sin_port = htons(port);
  }
  return ret;
}

} // namespace dns_cache
