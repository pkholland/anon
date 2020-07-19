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

#include "udp_dispatch.h"
#include <arpa/inet.h>
#include "fiber.h"

udp_dispatch::udp_dispatch(int port_or_socket, bool is_socket, bool ipv6)
{
  // no SOCK_CLOEXEC since we inherit this socket down to the child
  // when we do a child swap
  int udd_port;
  if (is_socket) {
    sock_ = port_or_socket;
    struct sockaddr_in sin;
    socklen_t len = sizeof(sin);
    if (getsockname(sock_, (struct sockaddr *)&sin, &len) == -1)
      anon_throw(std::runtime_error, "getsockname(socket, (struct sockaddr *)&sin, &len) == -1, errno: " << errno_string());
    else
      port_num_ = ntohs(sin.sin_port);
  } else {
    port_num_ = port_or_socket;
    sock_ = socket(ipv6 ? AF_INET6 : AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (sock_ == -1)
      do_error("socket(AF_INET6, SOCK_DGRAM | SOCK_NONBLOCK, 0)");

    // bind to any address that will route to this machine
    struct sockaddr_in6 addr = {0};
    socklen_t sz;
    if (ipv6)
    {
      addr.sin6_family = AF_INET6;
      addr.sin6_port = htons(port_or_socket);
      addr.sin6_addr = in6addr_any;
      sz = sizeof(sockaddr_in6);
    }
    else
    {
      auto addr4 = (struct sockaddr_in*)&addr;
      addr4->sin_family = AF_INET;
      addr4->sin_port = htons(port_or_socket);
      addr4->sin_addr.s_addr = INADDR_ANY;
      sz = sizeof(sockaddr_in);
    }
    if (bind(sock_, (struct sockaddr *)&addr, sz) != 0)
    {
      close(sock_);
      do_error("bind(<AF_INET/6 SOCK_DGRAM socket>, <" << port_or_socket << ", in6addr_any/INADDR_ANY>, sizeof(...))");
    }
  }

  anon_log("udp port " << port_num_ << " bound to socket " << sock_);

  io_dispatch::epoll_ctl(EPOLL_CTL_ADD, sock_, EPOLLIN, this);
}

void udp_dispatch::io_avail(const struct epoll_event &event)
{
  if (event.events & EPOLLIN)
  {
    while (true)
    {
      struct sockaddr_storage host;
      socklen_t host_addr_size = sizeof(host);
      auto buff = get_avail_buff();
      auto dlen = recvfrom(sock_, &(*buff)[0], buff->size(), 0, (struct sockaddr *)&host, &host_addr_size);
      if (dlen == -1)
      {
#if ANON_LOG_NET_TRAFFIC > 1
        if (errno != EAGAIN)
          anon_log("recvfrom failed with errno: " << errno_string());
#endif
        return;
      }
      else if (dlen == sizeof(buff->size()))
      {
#if ANON_LOG_NET_TRAFFIC > 1
        anon_log("message too big! all " << sizeof(msgBuff) << " bytes consumed in recvfrom call");
#endif
      }
      else
      {
        std::weak_ptr<udp_dispatch> wp = shared_from_this();
        fiber::run_in_fiber([wp, buff, dlen, host, host_addr_size] {
          auto ths = wp.lock();
          if (ths)
          {
            ths->recv_msg(&(*buff)[0], dlen, &host, host_addr_size);
            ths->release_buff(buff);
          }
        });
      }
    }
  }
  else
    anon_log_error("udp_dispatch::io_avail called with no EPOLLIN. event.events = " << event_bits_to_string(event.events));
}

std::shared_ptr<std::vector<unsigned char>> udp_dispatch::get_avail_buff()
{
  std::unique_lock<std::mutex> l(mtx);
  if (free_buffs.size() > 0)
  {
    auto b = free_buffs.front();
    free_buffs.pop();
    return b;
  }
  return std::make_shared<std::vector<unsigned char>>(65536);
}

void udp_dispatch::release_buff(const std::shared_ptr<std::vector<unsigned char>>& buff)
{
  std::unique_lock<std::mutex> l(mtx);
  free_buffs.push(buff);
}
