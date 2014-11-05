
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
                          last_field_str(""),
                          last_value_str("")
                      {}
                      
                      http_request    request;
                      bool            message_complete;
                      
                      enum {
                        k_value,
                        k_field
                      };
                      int header_state;
                      std::string last_field_str;
                      std::string last_value_str;
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
                        c->request.headers[c->last_field_str] = c->last_value_str;
                        c->last_field_str = "";
                        c->last_value_str = "";
                      }
                      c->last_field_str += std::string(at,length);
                      c->header_state = pc::k_field;
                      return 0;
                    };
                                              
                    settings.on_header_value = [](http_parser* p, const char *at, size_t length)->int
                    {
                      pc* c = (pc*)p->data;
                      c->last_value_str += std::string(at,length);
                      c->header_state = pc::k_value;
                      return 0;
                    };
                                              
                    settings.on_headers_complete = [](http_parser* p)->int
                    {
                      pc* c = (pc*)p->data;
                      c->request.http_major = p->http_major;
                      c->request.http_minor = p->http_minor;
                      c->request.method = p->method;
                      if (c->header_state == pc::k_value)
                          c->request.headers[c->last_field_str] = c->last_value_str;
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
                    http_parser_init(&parser, HTTP_REQUEST);
                    parser.data = &pcallback;
                    
                    while (!pcallback.message_complete)
                    {
                      char  buf[1024];
                      auto bytes_read = pipe->read(&buf,sizeof(buf));
                      auto bytes_parsed = http_parser_execute(&parser, &settings, &buf[0], bytes_read);
                      if (!pcallback.message_complete && bytes_parsed != bytes_read) {
                        #if ANON_LOG_NET_TRAFFIC > 1
                        anon_log("invalid http received from: " << *(const struct sockaddr_storage*)src_addr << ", error: " << http_errno_description((enum http_errno)parser.http_errno));
                        #endif
                        return;
                      }
                    }
                    
                    http_reply reply;
                    
                    f((const http_request&)pcallback.request, reply);
                    
                    std::ostringstream rp;
                    rp << "HTTP/1.1 " << reply.get_status_code() << "\r\n";
                    for (auto it = reply.get_headers().begin(); it != reply.get_headers().end(); it++)
                      rp << it->first << ": " << it->second << "\r\n";
                    rp << "Content-Length: " << reply.get_body().length() << "\r\n\r\n";
                    
                    rp << reply.get_body();
                     
                    pipe->write(rp.str().c_str(), rp.tellp());
                    
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
    
    std::map<std::string, std::string>  headers;
    http_parser_url                     p_url;
    std::string                         url_str;
    
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

