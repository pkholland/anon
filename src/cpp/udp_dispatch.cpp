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

udp_dispatch::udp_dispatch(int udp_port)
{
  // no SOCK_CLOEXEC since we inherit this socket down to the child
  // when we do a child swap
  sock_ = socket(AF_INET6, SOCK_DGRAM | SOCK_NONBLOCK, 0);
  if (sock_ == -1)
    do_error("socket(AF_INET6, SOCK_DGRAM | SOCK_NONBLOCK, 0)");

  // bind to any address that will route to this machine
  struct sockaddr_in6 addr = {0};
  addr.sin6_family = AF_INET6;
  addr.sin6_port = htons(udp_port);
  addr.sin6_addr = in6addr_any;
  if (bind(sock_, (struct sockaddr *)&addr, sizeof(addr)) != 0)
  {
    close(sock_);
    do_error("bind(<AF_INET6 SOCK_DGRAM socket>, <" << udp_port << ", in6addr_any>, sizeof(addr))");
  }

  anon_log("listening for udp on port " << udp_port << ", socket " << sock_);

  io_dispatch::epoll_ctl(EPOLL_CTL_ADD, sock_, EPOLLIN, this);
}

void udp_dispatch::io_avail(const struct epoll_event &event)
{
  if (event.events & EPOLLIN)
  {

    unsigned char msgBuff[8192];
    while (true)
    {
      struct sockaddr_storage host;
      socklen_t host_addr_size = sizeof(struct sockaddr_storage);
      auto dlen = recvfrom(sock_, &msgBuff[0], sizeof(msgBuff), 0, (struct sockaddr *)&host, &host_addr_size);
      if (dlen == -1)
      {
#if ANON_LOG_NET_TRAFFIC > 1
        if (errno != EAGAIN)
          anon_log("recvfrom failed with errno: " << errno_string());
#endif
        return;
      }
      else if (dlen == sizeof(msgBuff))
      {
#if ANON_LOG_NET_TRAFFIC > 1
        anon_log("message too big! all " << sizeof(msgBuff) << " bytes consumed in recvfrom call");
#endif
      }
      else
        recv_msg(&msgBuff[0], dlen, &host, host_addr_size);
    }
  }
  else
    anon_log_error("udp_dispatch::io_avail called with no EPOLLIN. event.events = " << event_bits_to_string(event.events));
}
