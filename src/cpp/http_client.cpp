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

namespace {

struct pc
{
  pc(http_client_response* cr)
    : message_complete(false),
      cr(cr),
      header_state(k_field),
      last_field_len(0),
      last_value_len(0),
      last_field_start(0),
      last_value_start(0)
  {
  }
  
  http_client_response* cr;
  bool                  message_complete;
  
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

}

void http_client_response::parse(const pipe_t& pipe)
{
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
    pc* c = (pc*)p->data;
    c->cr->set_status(at,length);
    return 0;
  };
                      
  settings.on_header_field = [](http_parser* p, const char *at, size_t length)->int
  {
    pc* c = (pc*)p->data;
    if (c->header_state == pc::k_value) {
      // <field,value> complete, add new pair
      string_len fld(c->last_field_start,c->last_field_len);
      string_len val(c->last_value_start,c->last_value_len);
      c->cr->headers.headers[fld] = val;
      
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
    c->cr->set_http_major(p->http_major);
    c->cr->set_http_minor(p->http_minor);
    if (c->header_state == pc::k_value) {
      string_len fld(c->last_field_start,c->last_field_len);
      string_len val(c->last_value_start,c->last_value_len);
      c->cr->headers.headers[fld] = val;
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

  // done in a loop because certain response status codes
  // indicate that we should just try reading the next
  // message
  size_t  bsp = 0, bep = 0;
  
  while (true) {
    headers.init();
    pc pcallback(this);
    http_parser parser;
    parser.data = &pcallback;
    http_parser_init(&parser, HTTP_RESPONSE);
    
    // because of the "while (true)" loop, it is possible
    // that there is unparsed data in header_buf_ already
    // at this point.  If so, we scoot it to be the start
    // of the buffer
    if (bep > bsp) {
      memmove(&header_buf_[0], &header_buf_[bsp], bep-bsp);
      bep -= bsp;
      bsp = 0;
    }
    
    while (!pcallback.message_complete && !parser.http_errno) {
    
      // have we already filled all of header_buf_ without seeing the end of the headers?
      if (bsp == sizeof(header_buf_)) {
        #if ANON_LOG_NET_TRAFFIC > 1
        anon_log("invalid http response - headers bigger than " << sizeof(header_buf_) << " bytes");
        #endif
        throw std::runtime_error("http headers too big");
      }
    
      // if there is no un-parsed data in header_buf_ then read more
      if (bsp == bep)
        bep += pipe.read(&header_buf_[bep], sizeof(header_buf_)-bep);
        
      // call the joyent parser
      bsp += http_parser_execute(&parser, &settings, &header_buf_[bsp], bep-bsp);
    
    }
    
    if (!pcallback.message_complete && parser.http_errno) {
      #if ANON_LOG_NET_TRAFFIC > 1
      anon_log("invalid http received, error: " << http_errno_description((enum http_errno)parser.http_errno));
      #endif
      //anon_log("bsp: " << bsp << ", bep: " << bep);
      //header_buf_[bep] = 0;
      //anon_log("http data:\n" << &header_buf_[bsp]);
      throw fiber_io_error("http parser error");
    }
    
    // check for some standard error codes that we handle with special logic
    if (parser.status_code == 100) {
      #if ANON_LOG_NET_TRAFFIC > 1
      anon_log("received http response 100, will continue and read next response");
      #endif
      continue;
    }
    if (parser.status_code == 408)
      throw fiber_io_error("408 server response");   
    if (parser.status_code == 500)
      throw fiber_io_error("500 server response", 30/*backoff seconds*/, true/*close_socket_hint*/);
    if (parser.status_code == 502)
      throw fiber_io_error("502 server response", 30/*backoff seconds*/, true/*close_socket_hint*/);
    if (parser.status_code == 503) {
      // lame TODO note: 503's are allowed to have a Retry-After header containing
      // the server's hint about when a good time would be to try again.  Retry-After
      // can either specify a HTTP-date or a number of seconds.  This code doesn't
      // support the HTTP-date case, and will end up behaving as if there were no
      // Retry-After specified in that case.
      int secs;
      if (headers.contains_header("Retry-After")) {
        auto h = headers.get_header("Retry-After");
        auto p = h.ptr();
        if (h.len() > 0 && *p >= '0' && *p <= '9')
          secs = atoi(p);
        else
          secs = 30;
      } else
        secs = 30;
      throw fiber_io_error("503 server response", secs, true/*close_socket_hint*/);
    }
    if (parser.status_code == 504)
      throw fiber_io_error("504 server response", 30/*backoff seconds*/, true/*close_socket_hint*/);
      
    // response is good enough to return
    status_code = parser.status_code;
    break;
  }
  
  auto clen = atoi(headers.get_header("Content-Length").ptr());
  body = std::vector<char>(clen);
  
  if (bep-bsp > clen) {
    #if ANON_LOG_NET_TRAFFIC > 1
    anon_log("ignoring " << bep-bsp-clen << " bytes of extra data in http response");
    #endif
    bep = bsp+clen;
  }
  
  // we might have read some, or all of the body already
  // in the loop above, so start by copying that
  memcpy(&body[0], &header_buf_[bsp], bep-bsp);

  // read the rest of the body of the message
  size_t pos = bep-bsp;
  while (pos < clen)
    pos += pipe.read(&body[pos], clen-pos);
    
}

