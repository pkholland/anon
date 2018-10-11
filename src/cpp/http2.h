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

#include "http_server.h"
#include "proxygen/lib/http/codec/compress/HPACKDecoder.h"
#include "proxygen/lib/http/codec/compress/HPACKEncoder.h"

class http2
{
public:
  // if/when HTTP/2 becomes a final standard and
  // anon fully implements it, then this will become
  // just "h2c".  For now, it is implementing draft 15
  // of the IETF spec. and calls its protocol
  // "h2c-15-anon"
  static const char *http2_name /* = "h2c-15-anon"*/;

  // 'f' argument is function to run when the other side
  // of the HTTP/2 connection sends a HEADERS or PUSH_PROMISE
  // to open a new stream
  template <typename Fn>
  http2(bool client /*vs. server*/, Fn f, size_t stack_size = fiber::k_default_stack_size)
      : req_enc_(proxygen::HPACK::MessageType::REQ, true),
        req_dec_(proxygen::HPACK::MessageType::REQ),
        resp_enc_(proxygen::HPACK::MessageType::RESP, true),
        resp_dec_(proxygen::HPACK::MessageType::RESP),
        stream_handler_factory_(std::unique_ptr<stream_handler_factory>(new strm_hand_fact<Fn>(f))),
        stack_size_(stack_size),
        next_stream_id_(client ? 3 : 2)
  {
  }

  void run(http_server::pipe_t &pipe);

  // frame types:
  // note, these are the constants defined in section 6 of the IETF
  // spec and so cannot be changed.
  enum
  {
    k_DATA,
    k_HEADERS,
    k_PRIORITY,
    k_RST_STREAM,
    k_SETTINGS,
    k_PUSH_PROMISE,
    k_PING,
    k_GOAWAY,
    k_WINDOW_UPDATE,
    k_CONTINUATION
  };
  enum
  {
    k_frame_header_size = 9
  };

  // defined settings parameters
  // note, these are the constants defined in section 6.5.2 of the IETF
  // spec and so cannot be changed.
  typedef enum
  {
    k_SETTINGS_HEADER_TABLE_SIZE = 1,
    k_SETTINGS_ENABLE_PUSH,
    k_SETTINGS_MAX_CONCURRENT_STREAMS,
    k_SETTINGS_INITIAL_WINDOW_SIZE,
    k_SETTINGS_MAX_FRAME_SIZE,
    k_SETTINGS_MAX_HEADER_LIST_SIZE,

    k_num_settings_params = k_SETTINGS_MAX_HEADER_LIST_SIZE
  } settings_t;
  enum
  {
    k_settings_ack = 1
  };

  enum
  {
    k_HEADERS_END_STREAM = 0x1,
    k_HEADERS_END_HEADERS = 0x4,
    k_HEADERS_PADDED = 0x8,
    k_HEADERS_PRIORITY = 0x20
  };

  typedef proxygen::HPACKHeader hpack_header;

  static void format_frame(char *buf, uint32_t length, uint8_t type, uint8_t flags, uint32_t stream_id);

  static void send_data();
  static void send_headers();
  static void send_priority();
  static void send_reset_stream();

  static void send_settings(http_server::pipe_t &pipe, uint32_t stream_id, const std::vector<std::pair<settings_t, uint32_t>> &settings = std::vector<std::pair<settings_t, uint32_t>>());

  static void send_push_promise();
  static void send_ping();
  static void send_goaway();
  static void send_window_update();
  static void send_continuation();

  static void parse_settings(uint32_t stream_id, uint8_t flags, uint32_t frame_size);

  // open a new stream (send the request to the peer)
  // with the given headers.  Returns the stream_id for this new
  // stream.  If is_headers is true then it will be opened with
  // k_HEADERS, otherwise it is opened with k_PUSH_PROMISE
  uint32_t open_stream(http_server::pipe_t &pipe, std::vector<http2::hpack_header> &headers, bool is_headers /*vs. is_push_promise*/);

private:
  proxygen::HPACKEncoder req_enc_;
  proxygen::HPACKDecoder req_dec_;
  proxygen::HPACKEncoder resp_enc_;
  proxygen::HPACKDecoder resp_dec_;
  uint32_t next_stream_id_;

  struct stream_handler
  {
    virtual ~stream_handler() {}
    virtual void exec(int read_fd, http_server::pipe_t &write_pipe, uint32_t stream_id) = 0;
  };

  template <typename Fn>
  struct strm_hand : public stream_handler
  {
    strm_hand(Fn f) : f_(f) {}
    virtual void exec(int read_fd, http_server::pipe_t &write_pipe, uint32_t stream_id)
    {
      f_(std::unique_ptr<fiber_pipe>(new fiber_pipe(read_fd, fiber_pipe::unix_domain)), write_pipe, stream_id);
    }
    Fn f_;
  };

  struct stream_handler_factory
  {
    virtual ~stream_handler_factory() {}
    virtual stream_handler *new_handler() = 0;
  };

  template <typename Fn>
  struct strm_hand_fact : public stream_handler_factory
  {
    strm_hand_fact(Fn f) : f_(f) {}

    virtual stream_handler *new_handler()
    {
      return new strm_hand<Fn>(f_);
    }

    Fn f_;
  };

  std::unique_ptr<stream_handler_factory> stream_handler_factory_;
  size_t stack_size_;
};
