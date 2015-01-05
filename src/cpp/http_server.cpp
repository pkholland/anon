/*
 Copyright (c) 2014 Anon authors, see AUTHORS file.
 
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

void http_server::start_(int tcp_port, body_handler* base_handler, int listen_backlog)
{
  auto server = new tcp_server(tcp_port,
  
    [base_handler, this](std::unique_ptr<fiber_pipe>&& pipe, const sockaddr* src_addr, socklen_t src_addr_len){
    
      // parser callback struct used as "user data"
      // style communcation in the callbacks so we
      // know what we are doing
      struct pc
      {
        pc(const sockaddr* src_addr, socklen_t src_addr_len)
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
        
        http_request    request;
        bool            message_complete;
        
        enum {
          k_value,
          k_field
        };
        int header_state;
        const char* last_field_start;
        size_t      last_field_len;
        const char* last_value_start;
        size_t      last_value_len;
      };
      pc pcallback(src_addr, src_addr_len);
    
      // joyent data structure, set its callback functions
      http_parser_settings settings;
      settings.on_message_begin = [](http_parser* p)->int
      {
        return 0;
      };
                                  
      settings.on_url = [](http_parser* p, const char *at, size_t length)->int
      {
        pc* c = (pc*)p->data;
        c->request.url_str += std::string(at,length);
        http_parser_parse_url(at, length, true, &c->request.p_url);
        return 0;
      };
                        
      settings.on_status = [](http_parser* p, const char *at, size_t length)->int
      {
        #if ANON_LOG_NET_TRAFFIC > 1
        anon_log("http status ignored since this should be a GET, status: \"" << std::string(at,length) << "\"");
        #endif
        return 0;
      };
                          
      settings.on_header_field = [](http_parser* p, const char *at, size_t length)->int
      {
        pc* c = (pc*)p->data;
        if (c->header_state == pc::k_value) {
          // <field,value> complete, add new pair
          http_headers::string_len fld(c->last_field_start,c->last_field_len);
          http_headers::string_len val(c->last_value_start,c->last_value_len);
          c->request.headers.headers[fld] = val;
          
          c->last_field_start = at;
          c->last_field_len = 0;
          c->header_state = pc::k_field;
        } else if (!c->last_field_start)
          c->last_field_start = at;
        c->last_field_len += length;
        return 0;
      };
                                
      settings.on_header_value = [](http_parser* p, const char *at, size_t length)->int
      {
        pc* c = (pc*)p->data;
        if (c->header_state == pc::k_field) {
          c->last_value_start = at;
          c->last_value_len = 0;
          c->header_state = pc::k_value;
        }
        c->last_value_len += length;
        return 0;
      };
                                
      settings.on_headers_complete = [](http_parser* p)->int
      {
        pc* c = (pc*)p->data;
        c->request.http_major = p->http_major;
        c->request.http_minor = p->http_minor;
        c->request.method = p->method;
        if (c->header_state == pc::k_value) {
          http_headers::string_len fld(c->last_field_start,c->last_field_len);
          http_headers::string_len val(c->last_value_start,c->last_value_len);
          c->request.headers.headers[fld] = val;
        }
        
        // note, see code in http_parser.c, returning 1 causes the
        // parser to skip attempting to read the body, which is
        // what we want.
        return 1;
      };
                                
      settings.on_body = [](http_parser* p, const char *at, size_t length)->int
      {
        return 1;
      };
                          
      settings.on_message_complete = [](http_parser* p)->int
      {
        pc* c = (pc*)p->data;
        c->message_complete = true;
        return 0;
      };
    
      http_parser parser;
      parser.data = &pcallback;
      http_parser_init(&parser, HTTP_REQUEST);
      
      bool    keep_alive = true;
      char    buf[4096];
      size_t  bsp = 0, bep = 0;
      while (keep_alive) {
      
        // if there is no un-parsed data in buf then read more
        if (bsp == bep)
          bep += pipe->read(&buf[bep], sizeof(buf)-bep);
          
        // call the joyent parser
        bsp += http_parser_execute(&parser, &settings, &buf[bsp], bep-bsp);
        
        if (pcallback.message_complete) {
        
          pipe_t body_pipe(pipe.get(), buf, bsp, bep);
        
          // did the headers indicate an upgrade?
          // if so, handle that differently
          if (parser.upgrade) {
          
            auto handler = m_upgrade_map_.find(pcallback.request.headers.get_header("Upgrade").str());
            if (handler != m_upgrade_map_.end())
              handler->second->exec(body_pipe, pcallback.request);
            else {
              #if ANON_LOG_NET_TRAFFIC > 1
              anon_log("unknown http upgrade type: \"" << pcallback.request.headers.get_header("Upgrade").str() << "\"");
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
          if (keep_alive) {
            http_parser_init(&parser, HTTP_REQUEST);
            memmove(&buf[0], &buf[bsp], bep-bsp);
            bep -= bsp;
            bsp = 0;
            pcallback.init();
          }
        
        } else {
        
          if (bsp != bep) {
            #if ANON_LOG_NET_TRAFFIC > 1
            anon_log("invalid http received from: " << *src_addr << ", error: " << http_errno_description((enum http_errno)parser.http_errno));
            #endif
            return;
          }
          if (bsp == sizeof(buf)) {
            #if ANON_LOG_NET_TRAFFIC > 1
            anon_log("http GET from: " << *src_addr << " invalid headers - bigger than " << sizeof(buf) << " bytes");
            #endif
            return;
          }
          
        }
      
      }
                          
    }, listen_backlog);

  tcp_server_ = std::unique_ptr<tcp_server>(server);
  body_holder_ = std::unique_ptr<body_handler>(base_handler);
}

size_t http_server::pipe_t::read(void* buff, size_t len)
{
  if (bsp != bep)
  {
    if (len > bsp-bep)
      len = bsp-bep;
    memcpy(buff, &buf[bsp], len);
    bsp += len;
    return len;
  }
  return pipe->read(buff, len);
}

void http_server::pipe_t::respond(const http_response& response)
{
  std::ostringstream rp;
  rp << "HTTP/1.1 " << response.get_status_code() << "\r\n";
  for (auto it = response.get_headers().begin(); it != response.get_headers().end(); it++)
    rp << it->first << ": " << it->second << "\r\n";
  if (response.get_body().length()) {
    rp << "Content-Length: " << response.get_body().length() << "\r\n\r\n";
    rp << response.get_body() << "\r\n";
  }
  rp << "\r\n";
   
  pipe->write(rp.str().c_str(), rp.tellp());
}


