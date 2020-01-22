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

#include "http_client.h"
#include <algorithm>

namespace
{

struct pc
{
  pc(http_client_response *cr, bool read_body)
      : message_complete(false),
        cr(cr),
        header_state(k_field),
        last_field_len(0),
        last_value_len(0),
        last_field_start(0),
        last_value_start(0),
        headers_complete_(false),
        cur_chunk_pos_(0),
        read_body_(read_body)
  {
  }

  http_client_response *cr;
  bool message_complete;

  enum
  {
    k_value,
    k_field
  };
  int header_state;
  const char *last_field_start;
  size_t last_field_len;
  const char *last_value_start;
  size_t last_value_len;
  size_t cur_chunk_pos_;
  bool headers_complete_;
  bool read_body_;
};

} // namespace

void http_client_response::parse(const pipe_t &pipe, bool read_body, bool throw_on_server_error)
{
  // joyent data structure, set its callback functions
  http_parser_settings settings;
  settings.on_message_begin = [](http_parser *p) -> int {
    return 0;
  };

  settings.on_url = [](http_parser *p, const char *at, size_t length) -> int {
#if ANON_LOG_NET_TRAFFIC > 1
    anon_log("http url ignored since this should be a response, url: \"" << std::string(at, length) << "\"");
#endif
    return 0;
  };

  settings.on_status = [](http_parser *p, const char *at, size_t length) -> int {
    pc *c = (pc *)p->data;
    c->cr->set_status(at, length);
    return 0;
  };

  settings.on_header_field = [](http_parser *p, const char *at, size_t length) -> int {
    pc *c = (pc *)p->data;
    if (c->header_state == pc::k_value)
    {
      // <field,value> complete, add new pair
      std::transform(c->last_field_start, c->last_field_start + c->last_field_len,
        (char*)c->last_field_start,
        [](char c){ return std::tolower(c); });
      string_len fld(c->last_field_start, c->last_field_len);
      string_len val(c->last_value_start, c->last_value_len);
      c->cr->headers.headers[fld] = val;

      c->last_field_start = at;
      c->last_field_len = 0;
      c->header_state = pc::k_field;
    }
    else if (!c->last_field_start)
      c->last_field_start = at;
    c->last_field_len += length;
    return 0;
  };

  settings.on_header_value = [](http_parser *p, const char *at, size_t length) -> int {
    pc *c = (pc *)p->data;
    if (c->header_state == pc::k_field)
    {
      c->last_value_start = at;
      c->last_value_len = 0;
      c->header_state = pc::k_value;
    }
    c->last_value_len += length;
    return 0;
  };

  settings.on_headers_complete = [](http_parser *p) -> int {
    pc *c = (pc *)p->data;
    c->cr->set_http_major(p->http_major);
    c->cr->set_http_minor(p->http_minor);
    c->headers_complete_ = true;
    if (c->header_state == pc::k_value)
    {
      std::transform(c->last_field_start, c->last_field_start + c->last_field_len,
        (char*)c->last_field_start,
        [](char c){ return std::tolower(c); });
      string_len fld(c->last_field_start, c->last_field_len);
      string_len val(c->last_value_start, c->last_value_len);
      c->cr->headers.headers[fld] = val;
    }
    if (c->read_body_)
    {
      if (p->flags & F_CONTENTLENGTH)
        c->cr->body.push_back(std::vector<char>(p->content_length));
      return 0;
    }
    return 1;
  };

  settings.on_body = [](http_parser *p, const char *at, size_t length) -> int {
    pc *c = (pc *)p->data;
    if (!c->cr->body.size())
    {
#if ANON_LOG_NET_TRAFFIC > 1
      anon_log("invalid call to on_body - no current chunk");
#endif
      return 1;
    }
    std::vector<char> &cur_chunk = c->cr->body.back();
    auto avail = cur_chunk.size() - c->cur_chunk_pos_;
    if (length > avail)
    {
#if ANON_LOG_NET_TRAFFIC > 1
      anon_log("invalid call to on_body - too much data supplied");
#endif
      return 1;
    }
    memcpy(&cur_chunk[c->cur_chunk_pos_], at, length);
    c->cur_chunk_pos_ += length;
    return 0;
  };

  settings.on_message_complete = [](http_parser *p) -> int {
    pc *c = (pc *)p->data;
    c->message_complete = true;
    return 0;
  };

  settings.on_chunk_header = [](http_parser *p) {
    pc *c = (pc *)p->data;
    if (c->cur_chunk_pos_)
    {
#if ANON_LOG_NET_TRAFFIC > 1
      anon_log("invalid call to on_chunk_header - incomplete previous chunk");
#endif
      return 1;
    }
    c->cr->body.push_back(std::vector<char>(p->content_length));
    return 0;
  };

  settings.on_chunk_complete = [](http_parser *p) {
    pc *c = (pc *)p->data;
    if (!c->cr->body.size())
    {
#if ANON_LOG_NET_TRAFFIC > 1
      anon_log("invalid call to on_chunk_complete - no current chunk");
#endif
      return 1;
    }
    std::vector<char> &cur_chunk = c->cr->body.back();
    if (cur_chunk.size() != c->cur_chunk_pos_)
    {
#if ANON_LOG_NET_TRAFFIC > 1
      anon_log("invalid call to on_chunk_complete - incomplete current chunk");
#endif
      return 1;
    }
    c->cur_chunk_pos_ = 0;
    return 0;
  };

  // done in a loop because certain response status codes
  // indicate that we should just try reading the next
  // message
  size_t bsp = 0, bep = 0;

  std::vector<char> bdy_tmp(4096);

  while (true)
  {
    headers.init();
    pc pcallback(this, read_body);
    http_parser parser;
    parser.data = &pcallback;
    http_parser_init(&parser, HTTP_RESPONSE);

    // because of the "while (true)" loop, it is possible
    // that there is unparsed data in header_buf_ already
    // at this point.  If so, we scoot it to be the start
    // of the buffer
    if (bep > bsp)
    {
      memmove(&header_buf_[0], &header_buf_[bsp], bep - bsp);
      bep -= bsp;
      bsp = 0;
    }

    while (!pcallback.message_complete && !parser.http_errno)
    {

      // have we already filled all of header_buf_ without seeing the end of the headers?
      if (!pcallback.headers_complete_ && (bsp == sizeof(header_buf_)))
      {
        anon_throw(std::runtime_error, "invalid http response - headers bigger than " << sizeof(header_buf_) << " bytes");
      }

      // if there is no un-parsed data in our read buffer then read more
      if (bsp == bep)
      {
        if (!pcallback.headers_complete_)
          bep += pipe.read(&header_buf_[bep], sizeof(header_buf_) - bep);
        else
        {
          bep = pipe.read(&bdy_tmp[0], bdy_tmp.size());
          bsp = 0;
        }
      }

      // call the joyent parser
      bsp += http_parser_execute(&parser, &settings, !pcallback.headers_complete_ ? &header_buf_[bsp] : &bdy_tmp[bsp], bep - bsp);
    }

    if (!pcallback.message_complete && parser.http_errno)
    {
      //anon_log("bsp: " << bsp << ", bep: " << bep);
      //header_buf_[bep] = 0;
      //anon_log("http data:\n" << &header_buf_[bsp]);
      anon_throw(fiber_io_error, "invalid http received, error: " << http_errno_description((enum http_errno)parser.http_errno));
    }

    // check for some standard error codes that we handle with special logic
    if (parser.status_code == 100)
    {
#if ANON_LOG_NET_TRAFFIC > 1
      anon_log("received http response 100, will continue and read next response");
#endif
      continue;
    }
    if (throw_on_server_error) {
      if (parser.status_code == 408)
        anon_throw(fiber_io_error, "408 server response");
      if (parser.status_code == 500)
        anon_throw(fiber_io_error, "500 server response");
      if (parser.status_code == 502)
        anon_throw(fiber_io_error, "502 server response");
      if (parser.status_code == 503)
        anon_throw(fiber_io_error, "503 server response");
      if (parser.status_code == 504)
        anon_throw(fiber_io_error, "504 server response");
    }

    // response is good enough to return
    status_code = parser.status_code;
    if (status_code >= 200 && status_code < 300)
      should_keep_alive = http_should_keep_alive(&parser);
    break;
  }
}
