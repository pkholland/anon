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
#include "pipe.h"

struct http_client_response
{
  http_client_response()
      : http_major_(0),
        http_minor_(0),
        should_keep_alive(false),
        has_content_length(false),
        content_length(0)
  {
  }

  struct body_pipe_t
  {
    body_pipe_t(http_client_response *cr)
        : cr(cr)
    {
    }
    size_t read(void *buff, size_t len);

  private:
    http_client_response *cr;
  };
  friend struct body_pipe_t;

  body_pipe_t parse(const pipe_t &pipe, bool read_body, bool throw_on_server_error = true);

  int status_code;
  http_headers headers;
  std::list<std::vector<char>> body;
  bool should_keep_alive;

  void set_status(const char *ptr, size_t len) { status_ = std::string(ptr, len); }
  const std::string &get_status() const { return status_; }

  void set_http_major(int major) { http_major_ = major; }
  int get_http_major() const { return http_major_; }
  void set_http_minor(int minor) { http_minor_ = minor; }
  int get_http_minor() const { return http_minor_; }

  const char *get_header_buf() const { return &header_buf_[0]; }
  bool has_content_length;
  size_t content_length;

private:
  char header_buf_[4096];
  std::string status_;
  int http_major_;
  int http_minor_;
  size_t header_len_;
  size_t bsp;
  size_t bep;
  const pipe_t *pipe_;
};
