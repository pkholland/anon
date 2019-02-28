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
#include <sys/signalfd.h>

// getaddrinfo_a is buggy!
// it's not even clear that it has advantages over just
// creating our own thread and executing getaddrinfo in that
// thead.  getaddrinfo_a also creates threads.  For now, don't
// use that implementation.

#ifdef USE_GETADDRINFO_A
namespace
{

class pipe_reader
{
  struct gaicb_rec
  {
    struct gaicb cb;
    struct addrinfo hints;
    char portString[8];
    struct gaicb *cbr;
    char hostName[1];
  };

  pipe_reader(int sigNum, int fd)
      : fp(fd, fiber_pipe::unix_domain),
        sigNum(sigNum)
  {
  }

  void do_read()
  {
    if (completions.size() > 1)
      return;

    auto stack_size = 16 * 1024 - 256;
    fiber::run_in_fiber(
        [] {
          int n_readers = 1;

          do
          {
            struct signalfd_siginfo ssi;
            size_t rsz;
            if ((rsz = pr->fp.read(&ssi, sizeof(ssi))) != sizeof(ssi))
            {
              anon_log("strange number of bytes read from socket.  Requested sizeof(struct signalfd_siginfo) (" << sizeof(ssi) << "), but got " << rsz);
              continue;
            }

            if (ssi.ssi_code == SI_ASYNCNL)
            {
              /* received response from getaddrinfo_a */
              gaicb_rec *cbr = (gaicb_rec *)ssi.ssi_ptr;
              auto cbp = &cbr->cb;

              int ret = gai_error(cbp);
              if (ret == EAI_INPROGRESS)
              {
                anon_log_error("strange call with gai_error returning EAI_INPROGRESS");
                continue;
              }

              std::function<void(int, const std::vector<sockaddr_in6> &)> proc;
              {
                fiber_lock l(mtx);
                auto it = pr->completions.find(cbr);
                if (it == pr->completions.end())
                {
                  anon_log_error("unknown gaicb");
                  continue;
                }
                proc = it->second;
                pr->completions.erase(it);
                n_readers = pr->completions.size();
              }

              if (ret != 0)
              {
                anon_log_error("getaddrinfo_a completed with error: " << gai_strerror(ret));
                proc(ret, std::vector<sockaddr_in6>());
              }
              else
              {
#if defined(ANON_LOG_DNS_LOOKUP)
                int num_returns = 0;
                auto rslt = cbp->ar_result;
                while (rslt)
                {
                  ++num_returns;
                  rslt = rslt->ai_next;
                }
                anon_log("dns lookup returned " << num_returns << " result" << (num_returns > 1 ? "s:" : ":"));
                rslt = cbp->ar_result;
                while (rslt)
                {
                  anon_log("  " << *rslt->ai_addr);
                  rslt = rslt->ai_next;
                }
#endif

                std::vector<sockaddr_in6> addrs;
                auto addinf = cbp->ar_result;
                while (addinf)
                {
                  sockaddr_in6 addr;
                  size_t addrlen = (addinf->ai_addr->sa_family == AF_INET6) ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
                  memcpy(&addr, addinf->ai_addr, addrlen);
                  addrs.push_back(addr);
                  addinf = addinf->ai_next;
                }
                //freeaddrinfo(cbp->ar_result);
                proc(0, addrs);
              }
              //free(cbr);
            }
            else
            {
              anon_log("strange, unknown ssi_code: " << ssi.ssi_code);
            }

          } while (n_readers > 0);
        },
        stack_size, "getaddrinfo reader");
  }

  fiber_pipe fp;
  int sigNum;
  std::map<gaicb_rec *, std::function<void(int, const std::vector<sockaddr_in6> &)>> completions;

  static std::unique_ptr<pipe_reader> pr;
  static fiber_mutex mtx;

public:
  static void lookup(const char *host, int port, const std::function<void(int, const std::vector<sockaddr_in6> &)> &completion)
  {
    auto sz = sizeof(gaicb_rec) + strlen(host) + 1;
    gaicb_rec *cbr = (gaicb_rec *)malloc(sz);
    memset(cbr, 0, sz);

    // gai takes the port parameter as a string
    sprintf(&cbr->portString[0], "%d", port);

    // the sorts of endpoints we are looking for
    cbr->hints.ai_family = AF_UNSPEC; // use IPv4 or IPv6, whichever
    cbr->hints.ai_socktype = SOCK_STREAM;

    // what we are trying to look up,
    // where to store the result...
    memcpy(&cbr->hostName[0], host, strlen(host));
    cbr->cb.ar_name = &cbr->hostName[0];
    cbr->cb.ar_service = &cbr->portString[0];
    cbr->cb.ar_request = &cbr->hints;
    cbr->cbr = &cbr->cb;

    int ret;
    {
      fiber_lock l(mtx);
      if (!pr)
      {
        auto sigNum = io_dispatch::getSigNum();
        sigset_t sigs;
        sigemptyset(&sigs);
        sigaddset(&sigs, sigNum);
        pr = std::unique_ptr<pipe_reader>(new pipe_reader(sigNum, signalfd(-1, &sigs, SFD_NONBLOCK | SFD_CLOEXEC)));
      }
      pr->completions[cbr] = completion;

      struct sigevent sev = {};
      sev.sigev_notify = SIGEV_SIGNAL;
      sev.sigev_signo = pr->sigNum;
      sev.sigev_value.sival_ptr = &cbr->cb;

      anon_log("calling getaddrinfo_a");
      ret = getaddrinfo_a(GAI_NOWAIT, &cbr->cbr, 1, &sev);
      anon_log("back from calling getaddrinfo_a");
      if (ret == 0)
        pr->do_read();
      else
      {
        anon_log_error("getaddrinfo_a(GAI_NOWAIT, &cba, 1, &se) failed with error: " << gai_strerror(ret));
        pr->completions.erase(pr->completions.find(cbr));
      }
    }
    if (ret != 0)
      completion(ret, std::vector<sockaddr_in6>());
  }
};

std::unique_ptr<pipe_reader> pipe_reader::pr;
fiber_mutex pipe_reader::mtx;

// end of anonymous namespace
} // namespace
#endif

////////////////////////////////////////////////////////

namespace dns_lookup
{

std::pair<int, std::vector<sockaddr_in6>> get_addrinfo(const char *host, int port)
{
  int err;
  std::vector<sockaddr_in6> addrs;

#ifdef USE_GETADDRINFO_A

  fiber_cond cond;
  fiber_mutex mtx;
  bool done = false;

  pipe_reader::lookup(host, port, [&](int _err, const std::vector<sockaddr_in6> &_addrs) {
    fiber_lock l(mtx);
    err = _err;
    addrs = _addrs;
    done = true;
    cond.notify_all();
  });

  fiber_lock l(mtx);
  while (!done)
    cond.wait(l);

#else

  std::string host_ = host;
  int pipe[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, &pipe[0]) != 0)
    do_error("socketpair(AF_UNIX, SOCK_STREAM, 0, &pipe[0])");

  std::thread t(
      [host_, port, &pipe, &err, &addrs] {
        struct addrinfo hints = {};
        hints.ai_family = AF_UNSPEC; // use IPv4 or IPv6, whichever
        hints.ai_socktype = SOCK_STREAM;
        char portString[8];
        sprintf(&portString[0], "%d", port);
        struct addrinfo *result;
        err = getaddrinfo(host_.c_str(), &portString[0], &hints, &result);
        if (err == 0)
        {
          auto addinf = result;
          while (addinf)
          {
            sockaddr_in6 addr;
            size_t addrlen = (addinf->ai_addr->sa_family == AF_INET6) ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
            memcpy(&addr, addinf->ai_addr, addrlen);
            addrs.push_back(addr);
            addinf = addinf->ai_next;
          }
          freeaddrinfo(result);
          write(pipe[0], "done", 5);
          close(pipe[0]);
        }
      });

  fiber_pipe fp(pipe[1], fiber_pipe::unix_domain);
  char buff[10];
  fp.read(&buff[0], sizeof(buff));

  t.join();

#endif

  return std::make_pair(err, addrs);
}

} // namespace dns_lookup
