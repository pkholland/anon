/*
 Copyright (c) 2023 Anon authors, see AUTHORS file.
 
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

#include <sys/socket.h>
#include <memory>
#include "tls_context.h"
#include "fiber.h"

struct sctp_association;

class dtls_dispatch : public std::enable_shared_from_this<dtls_dispatch>
{
  std::shared_ptr<tls_context> dtls_context;
  int udp_fd;
  fiber_mutex dtls_mtx;
  std::map<sockaddr_storage, std::shared_ptr<sctp_association>> sctp_associations;
  io_dispatch::scheduled_task sweep_task;

  void sweep_inactive();

public:
  dtls_dispatch(const std::shared_ptr<tls_context>& dtls_context, int udp_fd);
  void register_association(const struct sockaddr_storage *sockaddr, uint16_t local_sctp_port, uint16_t remote_sctp_port);
  void recv_msg(const uint8_t *msg, ssize_t len,
                const struct sockaddr_storage *sockaddr);
};
