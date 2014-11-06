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

#pragma once

#include "tcp_server.h"
#include "http_parser.h"
#include "tcp_utils.h"

class http_server
{
public:
  template<typename Fn>
  http_server(int tcp_port, const src_addr_validator& validator, Fn f, int listen_backlog = tcp_server::k_default_backlog)
    : tcp_server_(tcp_port, validator,
                  [f](std::unique_ptr<fiber_pipe>&& pipe, const sockaddr* src_addr, socklen_t src_addr_len){
                  
                    struct pc
                    {
                      pc(const sockaddr* src_addr, socklen_t src_addr_len)
                        : request(src_addr, src_addr_len),
                          message_complete(false),
                          header_state(k_field),
                          last_field_len(0),
                          last_value_len(0)
                      {}
                      
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
                        http_request::string_len fld(c->last_field_start,c->last_field_len);
                        http_request::string_len val(c->last_value_start,c->last_value_len);
                        c->request.headers[fld] = val;
                        
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
                        http_request::string_len fld(c->last_field_start,c->last_field_len);
                        http_request::string_len val(c->last_value_start,c->last_value_len);
                        c->request.headers[fld] = val;
                      }
                      return 0;
                    };
                                              
                    settings.on_body = [](http_parser* p, const char *at, size_t length)->int
                    {
                      #if ANON_LOG_NET_TRAFFIC > 1
                      anon_log("http body ignored since this should be a GET, body: \"" << std::string(at,length > 20 ? 20 : length) << "...\"");
                      #endif
                      return 0;
                    };
                                        
                    settings.on_message_complete = [](http_parser* p)->int
                    {
                      ((pc*)p->data)->message_complete = true;
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
                        
                      bsp += http_parser_execute(&parser, &settings, &buf[bsp], bep-bsp);
                      
                      if (pcallback.message_complete) {
                       
                        http_reply reply;
                        f((const http_request&)pcallback.request, reply);
                        
                        std::ostringstream rp;
                        rp << "HTTP/1.1 " << reply.get_status_code() << "\r\n";
                        for (auto it = reply.get_headers().begin(); it != reply.get_headers().end(); it++)
                          rp << it->first << ": " << it->second << "\r\n";
                        rp << "Content-Length: " << reply.get_body().length() << "\r\n\r\n";
                        rp << reply.get_body();
                         
                        pipe->write(rp.str().c_str(), rp.tellp());
                        
                        keep_alive = http_should_keep_alive(&parser);
                        if (keep_alive) {
                          http_parser_init(&parser, HTTP_REQUEST);
                          memmove(&buf[0], &buf[bsp], bep-bsp);
                          bep -= bsp;
                          bsp = 0;
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
                          anon_log("http GET from: " << *src_addr << " invalid - bigger than " << sizeof(buf) << " bytes");
                          #endif
                          return;
                        }
                        
                      }
                    
                    }
                                        
                  }, listen_backlog)
  {}
  
  struct http_request
  {
    http_request(const sockaddr* src_addr, socklen_t src_addr_len)
      : src_addr(src_addr),
        src_addr_len(src_addr_len)
    {
      memset(&p_url,0,sizeof(p_url));
    }
    
    const sockaddr* src_addr;
    socklen_t       src_addr_len;
    int             http_major;
    int             http_minor;
    int             method;
    
    const char* method_str() const { return http_method_str((enum http_method)method); }
    
    struct string_len {
      string_len()
        : str_(""),
          len_(0)
      {}
     
      string_len(const char* str, size_t len)
        : str_(str),
          len_(len)
      {}
      
      // warning! can only be called with a literal
      // or some other str whose lifespan exceeds
      // the lifespan of this string_len.
      explicit string_len(const char* str)
        : str_(str),
          len_(strlen(str))
      {}
      
      bool operator<(const string_len& sl) const
      {
        size_t l = len_ < sl.len_ ? len_ : sl.len_;
        auto ret = memcmp(str_,sl.str_,l);
        if (ret != 0)
          return ret < 0;
        return len_ < sl.len_;
      }
      
      std::string str() const { return std::string(str_,len_); }
      const char* ptr() const { return str_; }
            
    private:
      const char* str_;
      size_t      len_;
    };
    
    string_len get_header(const char* field) const
    {
      auto it = headers.find(string_len(field));
      if (it != headers.end())
        return it->second;
      return string_len("");
    }
    
    std::map<string_len, string_len>  headers;
    http_parser_url                   p_url;
    std::string                       url_str;
    
    std::string get_url_field(enum http_parser_url_fields f) const
    {
      if (p_url.field_set & (1 << f))
        return url_str.substr(p_url.field_data[f].off,p_url.field_data[f].len);
      return "";
    }
  };
  
  struct http_reply
  {
    http_reply()
      : status_code_("200 OK")
    {}
    
    void set_status_code(const std::string& code) { status_code_ = code; }
    const std::string& get_status_code() const { return status_code_; }
    
    void add_header(const std::string& field, const std::string& value) { headers_[field] = value; }
    const std::map<std::string,std::string>& get_headers() const { return headers_; }
    
    const std::string get_body() const { return ostr_.str(); }
    
    template<typename T>
    http_reply& operator<<(const T& t)
    {
      ostr_ << t;
      return *this;
    }
    
  private:
    std::string status_code_;
    std::map<std::string, std::string> headers_;
    std::ostringstream ostr_;
  };
  
  void attach(io_dispatch& io_d)
  {
    tcp_server_.attach(io_d);
  }
  
private:
  tcp_server                            tcp_server_;
  
};

// helper
template<typename T>
T& operator<<(T& str, const http_server::http_request::string_len& sl)
{
  return str << sl.str();
}




