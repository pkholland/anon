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
  
    struct sockaddr_storage addr;
    socklen_t addr_len = sizeof(addr);
    int conn = accept4(listen_sock_, (struct sockaddr*)&addr, &addr_len, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (conn == -1) {
      // we can get EAGAIN because multiple id_d threads
      // can wake up from a single EPOLLIN event.
      // don't bother reporting those.
      if (errno != EAGAIN)
        anon_log_error("accept4(sock_, (struct sockaddr*)&addr, &addr_len, SOCK_NONBLOCK | SOCK_CLOEXEC)");
    } else {
      #if ANON_LOG_NET_TRAFFIC > 2
      anon_log("new tcp connection from addr: " << addr);
      #endif
      fiber::run_in_fiber([conn,addr,addr_len,this]{new_conn_->exec(conn,(struct sockaddr*)&addr,addr_len);});
    }
    
  } else
    anon_log_error("tcp_server::io_avail called with no EPOLLIN. event.events = " << event_bits_to_string(event.events));
}

