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

#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <queue>
#include "data_channel_dispatch.h"

class sctp_dispatch : public std::enable_shared_from_this<sctp_dispatch>
{
  uint16_t local_port;
  uint16_t remote_port;
  std::function<void(const uint8_t* msg, size_t len)> send_reply;
  std::shared_ptr<data_channel_dispatch> dcd;
  std::set<uint32_t> tsns;
  std::vector<std::string> chunks;
  std::vector<uint32_t> duplicate_tsns;
  uint32_t verification_tag{0};
  uint32_t last_complete_tsn{0};

  bool parse_sctp_chunks(const uint8_t* msg, ssize_t len);
  void send_acks();
  bool do_chunk_init(const uint8_t* init_data, ssize_t init_len);

public:
  sctp_dispatch(uint16_t local_port, uint16_t remote_port);
  void connect(std::function<void(const uint8_t* msg, size_t len)>&& send_reply);
  void recv_msg(const uint8_t *msg, ssize_t len);

};