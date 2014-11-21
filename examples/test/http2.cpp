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

#include "http2.h"
#include "log.h"
#include "tcp_client.h"
#include "http2_handler.h"
#include "http_parser.h"  // github.com/joyent/http-parser
#include <netdb.h>

void run_http2_test(int http_port)
{
  anon_log("sending http/2 upgrade to localhost:" << http_port);
  tcp_client::connect_and_run("localhost", http_port, [http_port](int err_code, std::unique_ptr<fiber_pipe>&& pipe){
    if (err_code == 0) {
    
      // request the upgrade
      std::ostringstream oss;
      oss << "GET / HTTP/1.1\r\nHost: localhost:8619\r\nConnection: Upgrade, HTTP2-Settings\r\n";
      oss << "Upgrade: " << http2_handler::http2_name << "\r\n";
      oss << "HTTP2-Settings: \r\n";  // base64url encoded empty data block is empty, so here we use an empty SETTINGS frame to get default values
      oss << "\r\n";
      
      std::string simple_msg = oss.str();
      pipe->write(simple_msg.c_str(), simple_msg.length());
      
      // the response starts with a normal HTTP/1.1 -style response, telling us,
      // amoung other things, whether the server was willing/able to upgrade to
      // HTTP/2, so parse and read that
      
      // parser callback struct used as "user data"
      // style communcation in the callbacks so we
      // know what we are doing
      struct pc
      {
        pc()
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
        }
        
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
        int         http_major;
        int         http_minor;
        http_headers headers;
      };
      pc pcallback;
    
      // joyent data structure, set its callback functions
      http_parser_settings settings;
      settings.on_message_begin = [](http_parser* p)->int
      {
        return 0;
      };
                                  
      settings.on_url = [](http_parser* p, const char *at, size_t length)->int
      {
        #if ANON_LOG_NET_TRAFFIC > 1
        anon_log("http url ignored since this should be a response, url: \"" << std::string(at,length) << "\"");
        #endif
        return 0;
      };
                        
      settings.on_status = [](http_parser* p, const char *at, size_t length)->int
      {
        anon_log("got status of: \"" << std::string(at,length) << "\"");
        return 0;
      };
                          
      settings.on_header_field = [](http_parser* p, const char *at, size_t length)->int
      {
        pc* c = (pc*)p->data;
        if (c->header_state == pc::k_value) {
          // <field,value> complete, add new pair
          http_headers::string_len fld(c->last_field_start,c->last_field_len);
          http_headers::string_len val(c->last_value_start,c->last_value_len);
          c->headers.headers[fld] = val;
          
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
        c->http_major = p->http_major;
        c->http_minor = p->http_minor;
        if (c->header_state == pc::k_value) {
          http_headers::string_len fld(c->last_field_start,c->last_field_len);
          http_headers::string_len val(c->last_value_start,c->last_value_len);
          c->headers.headers[fld] = val;
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

      }

      
      unsigned char frame[http2_handler::k_frame_header_size];
      size_t bytes_read = 0;
      while (bytes_read < sizeof(frame))
        bytes_read += pipe->read(&frame[bytes_read], sizeof(frame)-bytes_read);
                  
      uint32_t  netv = 0;
      memcpy(&((char*)&netv)[1], &frame[0], 3);
      uint32_t  frame_size = ntohl(netv);
      memcpy(&netv, &frame[5], 4);
      uint32_t  stream_id = ntohl(netv);
      uint8_t   type = frame[3];
      uint8_t   flags = frame[4];

      anon_log("received response frame of type: " << (int)type << ", with size: " << frame_size);

    } else
      anon_log("connect to localhost: " << http_port << " failed with error: " << (err_code > 0 ? error_string(err_code) : gai_strerror(err_code)));
  });
}

