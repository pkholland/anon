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

#include "tcp_client.h"
#include "tcp_server.h"
#include "dns_cache.h"
#include <netdb.h>
#include <netinet/tcp.h>

namespace tcp_client
{

std::pair<int, std::unique_ptr<fiber_pipe>> connect(const struct sockaddr *addr, socklen_t addrlen)
{
  int fd = socket(addr->sa_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd == -1) {
    anon_log("socket(addr->sa_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0) faied, err: " << error_string(errno));
    return std::make_pair(errno, std::unique_ptr<fiber_pipe>());
  }

  std::unique_ptr<fiber_pipe> pipe(new fiber_pipe(fd, fiber_pipe::network));
  // anon_log("connecting new socket, fd: " << fd);
  auto cr = ::connect(fd, addr, addrlen);

  if (cr == 0)
  {
    anon_log("a little weird, but ok.  non-blocking connect succeeded immediately");
  }
  else if (errno != EINPROGRESS)
  {
    anon_log("connect(fd, " << *addr << ", addrlen) failed, err: " << error_string(errno));
    return std::make_pair(errno, std::unique_ptr<fiber_pipe>());
  }
  else
  {
    // fiber-sleep until connect completes
    io_params::sleep_cur_until_write_possible(pipe.get());

    // anon_log("new socket connected, fd: " << fd);

    // did connect succeed or fail?
    int result;
    socklen_t optlen = sizeof(result);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &result, &optlen) != 0)
      do_error("getsockopt(fd, SOL_SOCKET, SO_ERROR, &result, &optlen)");

    if (result != 0)
    {

#if ANON_LOG_NET_TRAFFIC > 0
      anon_log("async connect(" << *addr << ") completed with error: " << (result > 0 ? error_string(result) : gai_strerror(result)));
#endif

      int flag = 1;
      if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) != 0)
        anon_log("setsockopt(fd, SOL_SOCKET, TCP_NODELAY,...) failed");


      return std::make_pair(result, std::unique_ptr<fiber_pipe>());
    }
  }

  return std::make_pair(0, std::move(pipe));
}

static void inform(tcp_caller *tcpc, int err_code)
{
  tcpc->exec(err_code, std::unique_ptr<fiber_pipe>());
}

void do_connect_and_run(const char *host, int port, tcp_caller *tcpc, size_t stack_size)
{
  dns_cache::lookup_and_run(host, port, [tcpc](int err_code, const struct sockaddr *addr, socklen_t addrlen) {
    std::unique_ptr<tcp_caller> td(tcpc);

    if (err_code != 0) {
      inform(tcpc, err_code);
      return;
    }
    else
    {

      int fd = socket(addr->sa_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
      if (fd == -1)
      {
        anon_log_error("socket(addr->sa_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)");
        inform(tcpc, errno);
        return;
      }

#if defined(ANON_LOG_DNS_LOOKUP)
      anon_log("initiating async connect() to " << *addr);
#endif

      auto cr = connect(fd, addr, addrlen);
      std::unique_ptr<fiber_pipe> pipe(new fiber_pipe(fd, fiber_pipe::network));

      if (cr == 0)
      {
        anon_log("a little weird, but ok.  non-blocking connect succeeded immediately");
      }
      else if (errno != EINPROGRESS)
      {
        inform(tcpc, errno);
        return;
      }
      else
      {
        // fiber-sleep until connect completes
        io_params::sleep_cur_until_write_possible(pipe.get());

        // did connect succeed or fail?
        int result;
        socklen_t optlen = sizeof(result);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &result, &optlen) != 0)
        {
          inform(tcpc, errno);
          return;
        }

        if (result != 0)
        {
#if ANON_LOG_NET_TRAFFIC > 0
          anon_log("async connect() with fd: " << fd << " completed with error: " << (result > 0 ? error_string(result) : gai_strerror(result)));
#endif

          inform(tcpc, result);
          return;
        }
      }

      // connect succeeded, call the functor
      tcpc->exec(0, std::move(pipe));
    }
  },
  stack_size);
}

std::pair<int, std::unique_ptr<fiber_pipe>> connect(const char *host, int port)
{
  int err = 0;
  fiber_cond cond;
  fiber_mutex mtx;
  std::unique_ptr<fiber_pipe> fp;

  auto f = [&err, &fp, &mtx, &cond](int err_code, std::unique_ptr<fiber_pipe> &&pipe) {
    fiber_lock lock(mtx);
    err = err_code;
    if (err == 0)
      fp = std::move(pipe);
    cond.notify_all();
  };

  tcp_caller *tcpc = new tcp_call<decltype(f)>(f);
  do_connect_and_run(host, port, tcpc, fiber::k_default_stack_size);

  {
    fiber_lock lock(mtx);
    while (!err && !fp)
      cond.wait(lock);
  }

  return std::make_pair(err, std::move(fp));
}

} // namespace tcp_client
