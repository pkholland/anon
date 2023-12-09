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

#include <functional>
#include <cinttypes>
#include <map>
#include <memory>
#include <string>

class data_channel_dispatch
{
  std::function<void(uint32_t tsn, const uint8_t* chunk, size_t len)> add_chunk;

  enum {
    k_data_chunk_header_size = 16
  };

  class data_channel_stream {
    int channel_type;
    int priority;
    int reliability;
    std::string label;
    std::string protocol;

  public:
    data_channel_stream(
      int channel_type,
      int priority,
      int reliability,
      std::string&& label,
      std::string&& protocol);
    void do_data(
      data_channel_dispatch* cd,
      uint32_t tsn,
      uint16_t stream_sequency_num,
      uint32_t ppid,
      const uint8_t* data,
      size_t len);
  };
  friend class data_channel_stream;

  std::map<int, std::shared_ptr<data_channel_stream>> streams;

  void do_dcep(uint32_t tsn, int stream_id, const uint8_t* data, size_t len);

public:

  data_channel_dispatch(std::function<void(uint32_t tsn, const uint8_t* chunk, size_t len)> add_chunk);
  void recv_data_chunk(const uint8_t *chunk, ssize_t chunk_len);
};