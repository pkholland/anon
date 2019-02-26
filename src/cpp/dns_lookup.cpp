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

#include "dns_lookup.h"
#include "log.h"
#include "tcp_utils.h"
#include <netdb.h>

namespace
{

void inform_in_fiber(const std::function<void(int err_code, const std::vector<sockaddr_in6> &addrs)> &dnsc, int err)
{
  fiber::run_in_fiber(
      [dnsc, err] {
        dnsc(err, std::vector<sockaddr_in6>());
      },
      fiber::k_default_stack_size, "dns_lookup failure");
}

// structure used to hold state data
// during the getaddrinfo_a lookup
struct notify_complete
{
  notify_complete(const char *host, int port, const std::function<void(int err_code, const std::vector<sockaddr_in6> &addrs)> &dnsc, size_t stack_size)
      : host_(host),
        dnsc_(dnsc),
        stack_size_(stack_size)
  {
    // we don't need a very big stack for the
    // thread that libanl will use to call us
    // back on, so set the pthread stack size
    // small here
    int rslt = pthread_attr_init(&ptattr_);
    if (rslt != 0)
    {
      // note this is almost "do_error" except that macro uses errno instead of a provided rslt, so we do it manually here...
      anon_log_error("pthread_attr_init(&ptattr_) failed with result: " << error_string(rslt));
      inform_in_fiber(dnsc, rslt);
      throw std::system_error(rslt, std::system_category());
    }
    rslt = pthread_attr_setstacksize(&ptattr_, 128 * 1024);
    if (rslt != 0)
    {
      // note this is almost "do_error" except that macro uses errno instead of a provided rslt, so we do it manually here...
      anon_log_error("pthread_attr_setstacksize(&nc->ptattr_, 128*1024) failed with result: " << error_string(rslt));
      pthread_attr_destroy(&ptattr_);
      inform_in_fiber(dnsc, rslt);
      throw std::system_error(rslt, std::system_category());
    }

    // gai takes the port parameter as a string
    sprintf(&portString_[0], "%d", port);

    // the sorts of endpoints we are looking for
    memset(&hints_, 0, sizeof(hints_));
    hints_.ai_family = AF_UNSPEC; // use IPv4 or IPv6, whichever
    hints_.ai_socktype = SOCK_STREAM;

    // what we are trying to look up,
    // where to store the result...
    cb_.ar_name = host_.c_str();
    cb_.ar_service = &portString_[0];
    cb_.ar_request = &hints_;

    // how to notify us when the lookup completes
    memset(&se_, 0, sizeof(se_));
    se_.sigev_notify = SIGEV_THREAD;
    se_.sigev_value.sival_ptr = this;
    se_.sigev_notify_function = &resolve_complete;
    se_.sigev_notify_attributes = &ptattr_;
  }

  ~notify_complete()
  {
    pthread_attr_destroy(&ptattr_);
  }

  static void resolve_complete(union sigval sv);

  std::string host_;
  const std::function<void(int err_code, const std::vector<sockaddr_in6> &addrs)> dnsc_;
  size_t stack_size_;
  struct gaicb cb_;
  struct addrinfo hints_;
  struct sigevent se_;
  pthread_attr_t ptattr_;
  char portString_[8];
};

// callback that getaddrinfo_a calls when
// async dns resolution completes
void notify_complete::resolve_complete(union sigval sv)
{
  // we set this when we called getaddrinfo_a
  // so read it back here
  std::unique_ptr<notify_complete> ths((notify_complete *)sv.sival_ptr);

  // why were we called back?
  int ret = gai_error(&ths->cb_);
  if (ret == EAI_INPROGRESS)
  {
    anon_log_error("strange call to resolve_complete with gai_error returning EAI_INPROGRESS");
    ths.release(); // don't delete in this case -- whatever this case is...
    return;
  }
  else if (ret != 0)
  {

#if defined(ANON_LOG_DNS_LOOKUP)
    anon_log_error("getaddrinfo_a completed with error: " << gai_strerror(ret));
#endif

    inform_in_fiber(ths->dnsc_, ret);
  }
  else
  {

    // dns lookup succeeded

#if defined(ANON_LOG_DNS_LOOKUP)
    int num_returns = 0;
    auto rslt = ths->cb_.ar_result;
    while (rslt)
    {
      ++num_returns;
      rslt = rslt->ai_next;
    }
    anon_log("dns lookup for \"" << ths->host_.c_str() << "\" returned " << num_returns << " result" << (num_returns > 1 ? "s:" : ":"));
    rslt = ths->cb_.ar_result;
    while (rslt)
    {
      anon_log("  " << *rslt->ai_addr);
      rslt = rslt->ai_next;
    }
#endif

    std::vector<sockaddr_in6> addrs;
    auto addinf = ths->cb_.ar_result;
    while (addinf)
    {
      sockaddr_in6 addr;
      size_t addrlen = (addinf->ai_addr->sa_family == AF_INET6) ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
      memcpy(&addr, addinf->ai_addr, addrlen);
      addrs.push_back(addr);
      addinf = addinf->ai_next;
    }
    freeaddrinfo(ths->cb_.ar_result);

    auto dnsc = ths->dnsc_;
    fiber::run_in_fiber(
        [dnsc, addrs] {
          dnsc(0, addrs);
        },
        ths->stack_size_, "dns lookup complete");
  }
}

// end of anonymous namespace
} // namespace

////////////////////////////////////////////////////////

namespace dns_lookup
{

void lookup_and_run(const char *host, int port, const std::function<void(int err_code, const std::vector<sockaddr_in6> &addrs)> &dnsc, size_t stack_size)
{
#if defined(ANON_LOG_DNS_LOOKUP)
  anon_log("starting dns lookup for \"" << host << "\", port " << port);
#endif

  std::string host_ = host;

  // there is some evidence that getaddrinfo_a is unhappy if we
  // run it from one of our fibers.  It is also going to create a few
  // of its own threads when it executes.  So we start by creating
  // a thread on which we will call getaddrinfo_a.
  std::thread(
      [host_, port, dnsc, stack_size] {
        // The info we will need when resolve_complete is called.
        // Also, while the man pages for getaddrinfo_a aren't
        // particularly clear on this, the pointers given to it
        // in the various data structures need to _stay valid_
        // until the callback function is called.  So we can't
        // put these data structures on the stack.  They are members
        // of the notify_complete object, which is kept around until
        // we are called back.
        std::unique_ptr<notify_complete> nc(new notify_complete(host_.c_str(), port, dnsc, stack_size));

        // this one is ok to put on the stack
        struct gaicb *cba = &nc->cb_;

        // start the async getaddrinfo lookup
        int ret = getaddrinfo_a(GAI_NOWAIT, &cba, 1, &nc->se_);

        if (ret != 0)
        {
          anon_log_error("getaddrinfo_a(GAI_NOWAIT, &cba, 1, &se) failed with error: " << gai_strerror(ret));
          inform_in_fiber(dnsc, ret);
        }
        else
          nc.release(); // if getaddrinfo_a succeeded then nc is "owned" by notify_complete::resolve_complete
      })
      .detach();
}

std::pair<int, std::vector<sockaddr_in6>> get_addrinfo(const char *host, int port)
{
  fiber_cond cond;
  fiber_mutex mtx;
  bool done = false;
  int ret;
  std::vector<sockaddr_in6> addrs;
  auto stack_size = 16 * 1024 - 256;

  lookup_and_run(host, port,
                 [&ret, &addrs, &done, &mtx, &cond](int err_code, const std::vector<sockaddr_in6> &addrsp) {
                   ret = err_code;
                   if (ret == 0)
                     addrs = addrsp;

                   fiber_lock lock(mtx);
                   done = true;
                   cond.notify_all();
                 },
                 stack_size);

  fiber_lock lock(mtx);
  while (!done)
    cond.wait(lock);

  return std::make_pair(ret, addrs);
}

} // namespace dns_lookup
