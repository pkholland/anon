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

#include "tcp_server.h"
#include "http_parser.h"  // github.com/joyent/http-parser
#include "tcp_utils.h"
#include "tls_context.h"
#include "string_len.h"

struct http_headers
{  
  string_len get_header(const char* field) const
  {
    auto it = headers.find(string_len(field));
    if (it != headers.end())
      return it->second;
    return string_len("");
  }
  
  bool contains_header(const char* field) const
  {
    return headers.find(string_len(field)) != headers.end();
  }
  
  void init()
  {
    headers.clear();
  }
  
  std::map<string_len, string_len>  headers;
};

// method, headers, url, etc... (not body)
struct http_request
{
  http_request(const sockaddr* src_addr, socklen_t src_addr_len)
    : src_addr(src_addr),
      src_addr_len(src_addr_len)
  {
    memset(&p_url,0,sizeof(p_url)); // joyent data structure
  }
    
  const char* method_str() const { return http_method_str((enum http_method)method); }
  
  http_parser_url                   p_url;
  std::string                       url_str;
  
  std::string get_url_field(enum http_parser_url_fields f) const
  {
    if (p_url.field_set & (1 << f))
      return url_str.substr(p_url.field_data[f].off,p_url.field_data[f].len);
    return "";
  }
  
  static std::string get_query_val_s(const string_len& query, const char* field, const char* dflt = "", bool required = false)
  {
    auto len = strlen(field);
    const char* qs = query.ptr();
    const char* qse = &qs[query.len()];
    while (qs+len+1 < qse) {
      if (!memcmp(qs, field, len) && qs[len] == '=') {
        auto vs = qs+len+1;
        const char* ampr = (char*)memchr(vs, '&', qse-vs);
        if (!ampr)
          ampr = qse;
        return std::string(vs, ampr-vs);
      }
      const char* ampr = (char*)memchr(qs, '&', qse-qs);
      if (!ampr)
        ampr = qse;
      qs = ampr + 1;
    }
    if (required) {
      #if ANON_LOG_NET_TRAFFIC > 1
      anon_log("missing, required querystring field: \"" << field << "\"");
      #endif
      throw std::runtime_error("missing, required querystring field");
    }
    return dflt;
  }
  
  std::string get_query_val(const char* field, const char* dflt = "", bool required = false) const
  {
    return get_query_val_s((p_url.field_set & (1 << UF_QUERY)) ? string_len(&url_str.c_str()[p_url.field_data[UF_QUERY].off], p_url.field_data[UF_QUERY].len) : string_len(), field, dflt, required);
  }
  
  static void remove_query_field(std::string& uri, const char* field)
  {
    auto qs = strchr(uri.c_str(), '?');
    if (!qs)
      return;
      
    ++qs;
    auto qse = uri.c_str() + uri.length();
    auto len = strlen(field);
    
    while (qs+len+1 < qse) {
      if (!memcmp(qs, field, len) && (qs[len] == '=' || qs[len] == '&')) {
        std::string stripped(uri.c_str(), qs - uri.c_str());
        const char* ampr = (char*)strchr(qs+len, '&');
        if (ampr)
          stripped += std::string(ampr+1);                               // skip this & since ret already ends in either & or ?
        else
          stripped = std::string(stripped.c_str(), stripped.length()-1); // strip off the trailing & or ?
        uri = stripped;
        return;
      }
      qs = strchrnul(qs, '&') + 1;
    }
  }
  
  std::string get_cookie_val(const char* name) const
  {
    auto slen = strlen(name);
    auto cs = headers.get_header("Cookie");
    auto ptr = cs.ptr();
    auto end = ptr + cs.len();
    while (ptr < end) {
      auto e = ptr;
      while (e < end && *e != '=' && *e != ';')
        ++e;
      if (((e-ptr) == slen) && !memcmp(ptr, name, slen)) {
        if (e < end-1 && *e == '=') {
          ptr = ++e;
          while (e < end && *e != ';')
            ++e;
          return std::string(ptr, e-ptr);
        } else
          return "";
      }
      ptr = e;
      if (ptr < end) {
        ++ptr;  // skip '=' or ';', whichever it is
        if (ptr[-1] == '=') {
          // set ptr to end or the first char after the ';' (generally a ' ')
          while (ptr < end && *ptr != ';')
            ++ptr;
          if (ptr < end)
            ++ptr;
        }
        while (ptr < end && *ptr == ' ')
          ++ptr;
      }
    }
    return "";
  }
  
  void init()
  {
    headers.init();
    memset(&p_url,0,sizeof(p_url));
    url_str = "";
  }
  
  const sockaddr* src_addr;
  socklen_t       src_addr_len;
  http_headers    headers;
  int             http_major;
  int             http_minor;
  int             method;
};

class http_response
{
public:
  http_response(const std::string& status_code = "200 OK")
    : status_code_(status_code)
  {}
  
  void set_status_code(const std::string& code) { status_code_ = code; }
  const std::string& get_status_code() const { return status_code_; }
  
  void add_header(const std::string& field, const std::string& value) { headers_[field] = value; }
  const std::map<std::string,std::string>& get_headers() const { return headers_; }
  
  const std::string get_body() const { return ostr_.str(); }
  
  template<typename T>
  http_response& operator<<(const T& t)
  {
    ostr_ << t;
    return *this;
  }
  
private:
  std::string status_code_;
  std::map<std::string, std::string> headers_;
  std::ostringstream ostr_;
};

class http_server
{
public:  
  /*
    For all of the following functions that take a functor as an argument,
    this will be called whenever a new HTTP method has been parsed.  The
    prototype for that call must be:
    
      void Fn(http_server::pipe_t& pipe, const http_request& request)
      
    When this is called, 'request' will contain all of headers, the HTTP
    method, the client address, etc...  'pipe' will be positioned to the
    first byte of the body of the message if there is one.  So calling
    pipe.read() will begin reading the body.  A common behavior of an Fn
    function is to construct an http response of some kind and write it
    back to the client.  The simplest way to do this is to declare an
    http_response object, set it the way you want, and then call
    pipe.respond( <your response object> );
    
    If you want to support http "upgrades" (for example, switching
    the parsing logic from HTTP/1.1 to WebSockets, or HTTP/2), then you
    should use the default constructor for http_server, followed by
    a series of calls to 'add_upgrade_handler', then followed by a call
    to 'start'.  The http_server ctor that takes an Fn immediately calls
    start, and it is thread-unsafe to attempt to call add_upgrade_handler
    after start has been called.
    
    Declaring the http_server using the default ctor, not calling any
    add_upgrade_handler calls, and then calling start is equivalent to
    just using the ctor that takes an Fn.
  */

  // does _not_ start it running or associate with any port
  http_server()
  {}

  // run immediately with the given 'f'
  // NOTE, if tls_ctx != 0 it must remain valid for the life
  // of this http_server
  template<typename Fn>
  http_server(int tcp_port, Fn f, int listen_backlog = tcp_server::k_default_backlog,
            tls_context* tls_ctx = 0, bool port_is_fd = false)
  {
    start(tcp_port, f, listen_backlog, tls_ctx, port_is_fd);
  }
  
  // add the given 'f' as an upgrade handler, identified by 'name'.
  // if the HTTP headers contains:
  //
  //  Upgrade: <name>
  //
  // Then this 'f' will be called instead of the default 'f' passed to 'start'
  template<typename Fn>
  void add_upgrade_handler(const char* name, Fn f)
  {
    #if defined(ANON_RUNTIME_CHECKS)
    if (tcp_server_)
      do_error("add_upgrade_handler called after start");
    #endif
    m_upgrade_map_[name] = std::unique_ptr<body_handler>(new bod_hand<Fn>(f));
  }
  
  // used when you have constructed the http_server with the default ctor
  // and now want to start it running (presumably because you wanted to call
  // add_upgrade_handler prior to it starting)
  // NOTE, if tls_ctx != 0 it must remain valid for the life
  // of this http_server
  template<typename Fn>
  void start(int tcp_port, Fn f, int listen_backlog = tcp_server::k_default_backlog,
            tls_context* tls_ctx = 0, bool port_is_fd = false)
  {
    start_(tcp_port, new bod_hand<Fn>(f), listen_backlog, std::move(tls_ctx), port_is_fd);
  }
    
  struct pipe_t
  {
    pipe_t(::pipe_t* pipe, char (&buf)[4096], size_t& bsp, size_t& bep)
      : pipe(pipe),
        buf(buf),
        bep(bep),
        bsp(bsp)
    {}
    
    size_t read(void* buff, size_t len);
    
    void write(const void* buff, size_t len)
    {
      fiber_lock  lock(mutex);
      pipe->write(buff, len);
    }
    
    void respond(const http_response& response);
    
    int get_fd() const
    {
      return pipe->get_fd();
    }

  private:
    fiber_mutex mutex;
    ::pipe_t*   pipe;
    char        (&buf)[4096];
    size_t&     bsp;
    size_t&     bep;
  };
  
  void stop()
  {
    if (tcp_server_)
      tcp_server_->stop();
  }
  
private:

  struct body_handler
  {
    virtual ~body_handler() {}
    virtual void exec(http_server::pipe_t& pipe, const http_request& request) = 0;
  };
  
  template<typename Fn>
  struct bod_hand : public body_handler
  {
    bod_hand(Fn f) : f_(f) {}
    
    virtual void exec(http_server::pipe_t& pipe, const http_request& request)
    {
      f_(pipe, request);
    }
    
    Fn f_;
  };
  
  void start_(int tcp_port, body_handler* base_handler, int listen_backlog, tls_context* tls_ctx, bool port_is_fd);
  
  std::unique_ptr<tcp_server>   tcp_server_;
  std::unique_ptr<body_handler> body_holder_;
  std::map<std::string, std::unique_ptr<body_handler>> m_upgrade_map_;
};




