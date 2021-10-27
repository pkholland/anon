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

#include "tcp_server.h"
#include <mutex>
#include <netinet/tcp.h>

void tcp_server::init_socket(int tcp_port, int listen_backlog, bool port_is_fd)
{
  if (port_is_fd)
  {
    listen_sock_ = tcp_port;
  }
  else
  {
    listen_sock_ = socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
    if (listen_sock_ == -1)
      do_error("socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK, IPPROTO_TCP)");

    // bind to any address that will route to this machine
    struct sockaddr_in6 addr = {0};
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(tcp_port);
    addr.sin6_addr = in6addr_any;
    if (bind(listen_sock_, (struct sockaddr *)&addr, sizeof(addr)) != 0)
    {
      close(listen_sock_);
      do_error("bind(<AF_INET6 SOCK_STREAM socket>, <" << tcp_port << ", in6addr_any>, sizeof(addr))");
    }

    if (listen(listen_sock_, listen_backlog) != 0)
    {
      close(listen_sock_);
      do_error("listen(sock_, " << listen_backlog << ")");
    }
  }

  anon_log("listening for tcp connections on port " << get_port() << ", socket " << listen_sock_);

  // To get synchronization correct at stop time we use EPOLLONESHOT
  // which causes a slight performance penalty, since it requires that
  // we rearm the listening socket after each accept notification
  io_dispatch::epoll_ctl(EPOLL_CTL_ADD, listen_sock_, EPOLLIN | EPOLLONESHOT, this);
}

void tcp_server::io_avail(const struct epoll_event &event)
{
  if (event.events & EPOLLIN)
  {

    struct sockaddr_in6 addr;
    socklen_t addr_len = sizeof(addr);
    int conn = accept4(listen_sock_, (struct sockaddr *)&addr, &addr_len, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (conn == -1)
    {
      // we can get EAGAIN because multiple io_d threads
      // can wake up from a single EPOLLIN event.
      // don't bother reporting those.
      if (errno != EAGAIN) {
        anon_log_error("accept4(listen_sock_, (struct sockaddr*)&addr, &addr_len, SOCK_NONBLOCK | SOCK_CLOEXEC): " << error_string(errno));
        if (errno == EMFILE && !forced_close_) {
          forced_close_ = true;
          fiber::run_in_fiber([]{io_params::sweep_hibernating_pipes();});
        }
      }

      io_dispatch::epoll_ctl(EPOLL_CTL_MOD, listen_sock_, EPOLLIN | EPOLLONESHOT, this);
    }
    else
    {
      forced_close_ = false;
      if (stop_ && (addr == stop_addr_))
      {
        io_dispatch::epoll_ctl(EPOLL_CTL_DEL, listen_sock_, 0, this);
        fiber_lock lock(stop_mutex_);
        stop_ = false;
        stop_cond_.notify_all();
      }
      else
        io_dispatch::epoll_ctl(EPOLL_CTL_MOD, listen_sock_, EPOLLIN | EPOLLONESHOT, this);

#if ANON_LOG_NET_TRAFFIC > 2
      anon_log("new tcp connection on socket: " << conn << ", from addr: " << addr);
#endif
      fiber::run_in_fiber(
        [conn, addr, addr_len, this]
        {
          int flag = 1;
          if (setsockopt(conn, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) != 0)
            anon_log("setsockopt(conn, SOL_SOCKET, TCP_NODELAY,...) failed");
          new_conn_->exec(conn, (struct sockaddr *)&addr, addr_len);
        }, stack_size_, "tcp_server::io_avail");
    }
  }
  else
    anon_log_error("tcp_server::io_avail called with no EPOLLIN. event.events = " << event_bits_to_string(event.events));
}

void tcp_server::stop()
{
  memset(&stop_addr_, 0, sizeof(stop_addr_));
  stop_addr_.sin6_family = AF_INET6;
  stop_addr_.sin6_addr = in6addr_loopback;

  int fd = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd == -1)
    do_error("socket(addr->sa_family, SOCK_STREAM | SOCK_CLOEXEC, 0)");

  if (bind(fd, (struct sockaddr *)&stop_addr_, sizeof(stop_addr_)) != 0)
  {
    close(fd);
    do_error("bind(<AF_INET6 SOCK_STREAM socket>, <" << fd << ", in6addr_any>, sizeof(addr))");
  }

  socklen_t addrlen = sizeof(stop_addr_);
  if (getsockname(fd, (struct sockaddr *)&stop_addr_, &addrlen) != 0)
  {
    close(fd);
    do_error("getsockname(" << fd << ", addr, sizeof(addr))");
  }

  // now we have a bound socket in fd, and we have its addr in stop_addr_
  // so now set stop_ to true and connect to our listening socket.

  struct sockaddr_in6 addr;
  addrlen = sizeof(addr);
  if (getsockname(listen_sock_, (struct sockaddr *)&addr, &addrlen) != 0)
  {
    close(fd);
    do_error("getsockname(" << listen_sock_ << ", addr, sizeof(addr))");
  }

  stop_ = true;
  if (connect(fd, (const struct sockaddr *)&addr, addrlen) != 0)
  {
    close(fd);
    do_error("connect(" << fd << ", addr, sizeof(addr))");
  }

  close(fd);

  if (get_current_fiber_id() == 0)
  {

// this would be bad because we will stall this os thread
// until the fiber runs (which stalls until the server stops).
// if we are running with a single io_dispatch thread then
// this we would be on the os thread that all fibers run on,
// and so the fibers wouldn't run while this is stalled.
#if defined(ANON_RUNTIME_CHECKS)
    if (io_dispatch::is_io_dispatch_thread())
      anon_log("ILLEGAL and DANGEROUS call to tcp_server::stop from a raw io_dispatch thread!");
#endif

    std::mutex mtx;
    std::condition_variable cond;
    bool running = true;
    fiber::run_in_fiber([this, &mtx, &cond, &running] {
      {
        fiber_lock lock(stop_mutex_);
        while (stop_)
          stop_cond_.wait(lock);
      }
      {
        std::unique_lock<std::mutex> lock(mtx);
        running = false;
        cond.notify_one();
      }
    }, fiber::k_default_stack_size, "tcp_server::stop, wait for stop");
    std::unique_lock<std::mutex> lock(mtx);
    while (running)
      cond.wait(lock);
  }
  else
  {
    fiber_lock lock(stop_mutex_);
    while (stop_)
      stop_cond_.wait(lock);
  }
}

int tcp_server::get_port()
{
  struct sockaddr_in6 addr = {0};
  socklen_t addrlen = sizeof(addr);
  if (getsockname(listen_sock_, (struct sockaddr *)&addr, &addrlen) != 0)
    do_error("getsockname(" << listen_sock_ << ", (struct sockaddr*)&addr, &addrlen)");
  return ntohs(addr.sin6_port);
}
