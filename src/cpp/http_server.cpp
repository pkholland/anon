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

#include "http_server.h"
#include "tls_pipe.h"
#include <algorithm>

void http_server::start_(int tcp_port, body_handler *base_handler, int listen_backlog, tls_context *tls_ctx, bool port_is_fd, size_t stack_size)
{
  auto server = new tcp_server(
      tcp_port,

      [base_handler, this, tls_ctx](std::unique_ptr<fiber_pipe> &&pipe, const sockaddr *src_addr, socklen_t src_addr_len) {
        // parser callback struct used as "user data"
        // style communcation in the callbacks so we
        // know what we are doing
        struct pc
        {
          pc(const sockaddr *src_addr, socklen_t src_addr_len)
              : request(src_addr, src_addr_len)
          {
            init();
          }

          void init()
          {
            message_complete = false;
            header_state = k_field;
            last_field_len = 0;
            last_value_len = 0;
            last_field_start = 0;
            last_value_start = 0;
            request.init();
          }

          http_request request;
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
        };
        pc pcallback(src_addr, src_addr_len);

        // joyent data structure, set its callback functions
        http_parser_settings settings;
        settings.on_message_begin = [](http_parser *p) -> int {
          return 0;
        };

        settings.on_url = [](http_parser *p, const char *at, size_t length) -> int {
          pc *c = (pc *)p->data;
          c->request.url_str += std::string(at, length);
          auto &url = c->request.url_str;
          http_parser_parse_url(url.c_str(), url.length(), true, &c->request.p_url);
          return 0;
        };

        settings.on_status = [](http_parser *p, const char *at, size_t length) -> int {
#if ANON_LOG_NET_TRAFFIC > 1
          anon_log("http status ignored since this should be a GET, status: \"" << std::string(at, length) << "\"");
#endif
          return 0;
        };

        settings.on_header_field = [](http_parser *p, const char *at, size_t length) -> int {
          pc *c = (pc *)p->data;
          if (c->header_state == pc::k_value)
          {
            // <field,value> complete, convert field to lower case and add new pair
            std::transform(c->last_field_start, c->last_field_start + c->last_field_len,
              (char*)c->last_field_start,
              [](char c){ return std::tolower(c); });
            string_len fld(c->last_field_start, c->last_field_len);
            string_len val(c->last_value_start, c->last_value_len);
            c->request.headers.headers[fld] = val;

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
          c->request.http_major = p->http_major;
          c->request.http_minor = p->http_minor;
          c->request.method = p->method;
          if (c->header_state == pc::k_value)
          {
            std::transform(c->last_field_start, c->last_field_start + c->last_field_len,
              (char*)c->last_field_start,
              [](char c){ return std::tolower(c); });
            string_len fld(c->last_field_start, c->last_field_len);
            string_len val(c->last_value_start, c->last_value_len);
            c->request.headers.headers[fld] = val;
          }
          c->request.has_content_length = (p->flags & F_CONTENTLENGTH) != 0;
          c->request.content_length = p->content_length;

          // note, see code in http_parser.c, returning 1 causes the
          // parser to skip attempting to read the body, which is
          // what we want.
          return 1;
        };

        settings.on_body = [](http_parser *p, const char *at, size_t length) -> int {
          return 1;
        };

        settings.on_message_complete = [](http_parser *p) -> int {
          pc *c = (pc *)p->data;
          c->message_complete = true;
          return 0;
        };

        // require prompt navigation through the
        // initial tls handshake and http headers.
        // No stalling a single read for longer
        // than 4 seconds.
        pipe->limit_io_block_time(2);

        std::unique_ptr<tls_pipe> tlspipe;
        ::pipe_t *http_pipe;
        if (tls_ctx)
        {
          tlspipe = std::unique_ptr<tls_pipe>(new tls_pipe(std::move(pipe),
                                                           false /*client - we are a server*/,
                                                           false /*don't verify_peer*/,
                                                           false /*don't do SNI*/,
                                                           0 /*host_name*/,
                                                           *tls_ctx));
          http_pipe = tlspipe.get();
        }
        else
          http_pipe = pipe.get();

        http_parser parser;
        parser.data = &pcallback;
        http_parser_init(&parser, HTTP_REQUEST);

        bool keep_alive = true;
        std::vector<char>buf(8192);
        size_t bsp = 0, bep = 0;
        while (keep_alive)
        {
          // if there is no un-parsed data in buf then read more
          if (bsp == bep)
          {
            auto bytes_read = http_pipe->read(&buf[bep], buf.size() - bep);
            //anon_log("client sent: " << std::string(&buf[bep], bytes_read));
            bep += bytes_read;
          }

          http_pipe->set_hibernating(false);

          // call the joyent parser
          bsp += http_parser_execute(&parser, &settings, &buf[bsp], bep - bsp);

          if (pcallback.message_complete)
          {
            // requirement of prompt send's is a one-time
            // policy.  If we make it this far, then reset
            // the io blocking time to something more generous
            http_pipe->limit_io_block_time(15);

            pipe_t body_pipe(http_pipe, buf, bsp, bep);

            // did the headers indicate an upgrade?
            // if so, handle that differently
            if (parser.upgrade)
            {
              auto handler = m_upgrade_map_.find(pcallback.request.headers.get_header("upgrade").str());
              if (handler != m_upgrade_map_.end())
                handler->second->exec(body_pipe, pcallback.request);
              else
              {
#if ANON_LOG_NET_TRAFFIC > 1
                anon_log("unknown http upgrade type: \"" << pcallback.request.headers.get_header("upgrade").str() << "\"");
#endif
              }

              // an upgrade always results in final transfer
              // of the socket to the handler.  We never go
              // back and look at things like keep_alive again
              // here.  Once the handler returns (or if there
              // is no handler) we close the socket.
              return;
            }

            base_handler->exec(body_pipe, pcallback.request);

            keep_alive = http_should_keep_alive(&parser);
#if defined(ANON_FORCE_NO_KEEP_ALIVE)
            keep_alive = false;
#endif
            if (keep_alive)
            {
              http_parser_init(&parser, HTTP_REQUEST);
              memmove(&buf[0], &buf[bsp], bep - bsp);
              bep -= bsp;
              bsp = 0;
              pcallback.init();
              http_pipe->set_hibernating(true);
            }
          }
          else
          {

            if (bsp != bep)
            {
#if ANON_LOG_NET_TRAFFIC > 1
              anon_log("invalid http received from: " << *src_addr << ", error: " << http_errno_description((enum http_errno)parser.http_errno));
#endif
              return;
            }
            if (bsp == buf.size())
            {
#if ANON_LOG_NET_TRAFFIC > 1
              anon_log("http GET from: " << *src_addr << " invalid headers - bigger than " << buf.size() << " bytes");
#endif
              return;
            }
          }
        }
      },
      listen_backlog, port_is_fd, stack_size);

  tcp_server_ = std::unique_ptr<tcp_server>(server);
  body_holder_ = std::unique_ptr<body_handler>(base_handler);
}

size_t http_server::pipe_t::read(void *buff, size_t len)
{
  if (bsp != bep)
  {
    if (len > bep - bsp)
      len = bep - bsp;
    memcpy(buff, &buf[bsp], len);
    bsp += len;
    return len;
  }
  return pipe->read(buff, len);
}

void http_server::pipe_t::respond(const http_response &response)
{
  std::ostringstream rp;
  rp << "HTTP/1.1 " << response.get_status_code() << "\r\n";
  for (auto it = response.get_headers().begin(); it != response.get_headers().end(); it++)
    rp << it->first << ": " << it->second << "\r\n";
  for (auto it = response.get_cookies().begin(); it != response.get_cookies().end(); it++)
  {
    rp << "Set-Cookie: " << it->name_ << "=";
    if (!it->delete_it_)
      rp << it->value_;
    if (it->path_.size())
      rp << "; Path=" << it->path_;
    if (it->domain_.size())
      rp << "; Domain=" << it->domain_;
    if (it->delete_it_)
      rp << "; Max-Age=0";
    else if (it->max_age_ > 0)
      rp << "; Max-Age=" << it->max_age_;
    else if (it->max_age_ < 0)
      rp << "; expires=Thu, 01 Jan 1970 00:00:00 GMT";
    if (it->same_site_.size())
      rp << "; SameSite=" << it->same_site_;
    if (it->secure_ || it->same_site_ == "None")
      rp << "; Secure";
    if (it->http_only_)
      rp << "; HttpOnly";
    rp << "\r\n";
  }
  if (response.get_body().length() > 0)
    rp << "content-length: " << response.get_body().length() << "\r\n";
  rp << "\r\n";
  if (response.get_body().length())
    rp << response.get_body();

  auto buff = rp.str();
  auto len = buff.size();
  pipe->write(buff.c_str(), len);
}
