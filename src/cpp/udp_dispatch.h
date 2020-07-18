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

#pragma once

#include "io_dispatch.h"
#include <queue>
#include <mutex>

class udp_dispatch : public io_dispatch::handler, public std::enable_shared_from_this<udp_dispatch>
{
public:
  udp_dispatch(int port_or_socket, bool is_socket, bool ipv6 = false);

  ~udp_dispatch()
  {
    close(sock_);
  }

  // messages sent to this machine/port are passed to this
  // method.
  virtual void recv_msg(const unsigned char *msg, ssize_t len,
                        const struct sockaddr_storage *sockaddr,
                        socklen_t sockaddr_len) = 0;

  virtual void io_avail(const struct epoll_event &event);

  int get_sock() { return sock_; }

private:
  int sock_;
  std::queue<std::shared_ptr<std::vector<unsigned char>>> free_buffs;
  std::mutex mtx;

  std::shared_ptr<std::vector<unsigned char>> get_avail_buff();
  void release_buff(const std::shared_ptr<std::vector<unsigned char>>& buff);
};
