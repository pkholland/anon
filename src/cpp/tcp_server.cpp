/*
 Copyright (c) 2014 Anon authors, see AUTHORS file.
 
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

void tcp_server::init_socket(int tcp_port, int listen_backlog, bool port_is_fd)
{
  if (port_is_fd) {
    listen_sock_ = tcp_port;
    struct sockaddr_in6 addr = { 0 };
    socklen_t addrlen = sizeof(addr);
    if (getsockname(listen_sock_, (struct sockaddr*)&addr, &addrlen) != 0)
      do_error("getsockname(" << listen_sock_ << ", (struct sockaddr*)&addr, &addrlen)");
    tcp_port = ntohs(addr.sin6_port);
  } else {
    // no SOCK_CLOEXEC since we inherit this socket down to the child
    // when we do a child swap
    listen_sock_ = socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK, IPPROTO_TCP);
    if (listen_sock_ == -1)
      do_error("socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK, IPPROTO_TCP)");

    // bind to any address that will route to this machine
    struct sockaddr_in6 addr = { 0 };
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(tcp_port);
    addr.sin6_addr = in6addr_any;
    if (bind(listen_sock_, (struct sockaddr*)&addr, sizeof(addr)) != 0)
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

  anon_log("listening for tcp connections on port " << tcp_port << ", socket " << listen_sock_);
  
  io_dispatch::epoll_ctl(EPOLL_CTL_ADD, listen_sock_, EPOLLIN, this);
}

void tcp_server::io_avail(const struct epoll_event& event)
{
  if (event.events & EPOLLIN) {
  
    struct sockaddr_in6 addr;
    socklen_t addr_len = sizeof(addr);
    int conn = accept4(listen_sock_, (struct sockaddr*)&addr, &addr_len, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (conn == -1) {
      // we can get EAGAIN because multiple id_d threads
      // can wake up from a single EPOLLIN event.
      // don't bother reporting those.
      if (errno != EAGAIN)
        anon_log_error("accept4(sock_, (struct sockaddr*)&addr, &addr_len, SOCK_NONBLOCK | SOCK_CLOEXEC)");
    } else {
    
      if (stop_ && (addr == stop_addr_)) {
        io_dispatch::while_paused([this]{
          io_dispatch::epoll_ctl(EPOLL_CTL_DEL, listen_sock_, 0, this);
        });
        fiber_lock lock(stop_mutex_);
        stop_ = false;
        stop_cond_.notify_all();
      }
    
      #if ANON_LOG_NET_TRAFFIC > 2
      anon_log("new tcp connection from addr: " << addr);
      #endif
      fiber::run_in_fiber([conn,addr,addr_len,this]{new_conn_->exec(conn,(struct sockaddr*)&addr,addr_len);});
    }
    
  } else
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

  if (bind(fd, (struct sockaddr*)&stop_addr_, sizeof(stop_addr_)) != 0) {
    close(fd);
    do_error("bind(<AF_INET6 SOCK_STREAM socket>, <" << fd << ", in6addr_any>, sizeof(addr))");
  }
  
  socklen_t addrlen = sizeof(stop_addr_);
  if (getsockname(fd, (struct sockaddr *)&stop_addr_, &addrlen) != 0) {
    close(fd);
    do_error("getsockname(" << fd << ", addr, sizeof(addr))");
  }
  
  // now we have a bound socket in fd, and we have its addr in stop_addr_
  // so now set stop_ to true and connect to our listening socket.

  struct sockaddr_in6 addr;
  addrlen = sizeof(addr);
  if (getsockname(listen_sock_, (struct sockaddr *)&addr, &addrlen) != 0) {
    close(fd);
    do_error("getsockname(" << listen_sock_ << ", addr, sizeof(addr))");
  }
  
  stop_ = true;
  if (connect(fd, (const struct sockaddr *)&addr, addrlen) != 0) {
    close(fd);
    do_error("connect(" << fd << ", addr, sizeof(addr))");
  }
  
  close(fd);
  
  fiber_lock  lock(stop_mutex_);
  while (stop_)
    stop_cond_.wait(lock);
}


