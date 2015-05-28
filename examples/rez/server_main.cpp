/*
 Copyright (c) 2015 ANON authors, see AUTHORS file.
 
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

#include "resources.h"
#include "http_server.h"
#include <map>

class c_helper
{
public:
  virtual ~c_helper() {}
  virtual void call(http_server::pipe_t& pipe, const http_request& request) = 0;
};

template<typename T>
class c_helper_t : public c_helper
{
public:
  c_helper_t(T t)
    : t_(t)
  {}
  
  virtual void call(http_server::pipe_t& pipe, const http_request& request)
  {
    t_(pipe, request);
  }
  
  T t_;
};

std::map<std::string, std::map<std::string, c_helper*>> response_map;

template<typename T>
void add_mapping(const std::string& method, const std::string& path, T t)
{
  response_map[method][path] = new c_helper_t<T>(t);
}

// simple hack
static bool permits_gzip(const string_len& s)
{
  if (s.len() == 0)
    return false;
  auto p = s.ptr();
  auto ep = p+s.len();
  while (p < ep) {
    auto c = (const char*)memchr(p, ',', ep-p);
    if (!c)
      c = ep;
    if (((c >= p+4/*strlen("gzip")*/) && !memcmp(p, "gzip", 4)) || *p == '*') {
      p += *p == '*' ? 1 : 4;
      if (p < ep && *p == ';') {
        ++p;
        auto len = c-p;
        if (len >= 3/*strlen("q=0")*/ && memmem(p, len, "q=0", 3))
          return false;
      }
      return true;
    }
    p = c;
    while (p < ep && *p == ' ')
      ++p;
  }
  return false;
}

void server_init()
{
  for_each_rez([](const std::string& path, const rez_file_ent* ent){
    add_mapping("GET", path, [ent](http_server::pipe_t& pipe, const http_request& request){

      // note that if we get here we already know that the path maps to 'ent'
      // so just respond with that.  But this can't quite be a simple, static
      // response because we want to respect the etag, etc...
      
      anon_log("client sent If-None-Match: " << request.headers.get_header("If-None-Match").str());
      anon_log("client sent Accept-Encoding: " << request.headers.get_header("Accept-Encoding").str());

      if (request.headers.get_header("If-None-Match").str() == ent->etag) {
      
        anon_log("etags match, returning 304");
        pipe.respond(http_response("304 Not Modified"));
        
      } else {
        
        anon_log("etags do not match, returning 200 with " << (permits_gzip(request.headers.get_header("Accept-Encoding")) ? "gzip" : "identity") << " encoding");

        http_response response;
        response.add_header("ETag", ent->etag);
        response.add_header("Content-Type", ent->content_type);
        if (permits_gzip(request.headers.get_header("Accept-Encoding"))) {
          response.add_header("Content-Encoding", "gzip");
          response << std::string((const char*)ent->compressed, ent->sz_compressed);
        } else
          response << std::string((const char*)ent->uncompressed, ent->sz_uncompressed);
      
        pipe.respond(response);
      }
      
    });
  });
}

void server_respond(http_server::pipe_t& pipe, const http_request& request, bool is_tls)
{
  auto m = response_map.find(request.method_str()); // is it GET, POST, etc...?
  if (m != response_map.end()) {
    auto e = m->second.find(request.get_url_field(UF_PATH));
    if (e != m->second.end()) {
      e->second->call(pipe, request);
      return;
    }
  }
  
  anon_log("returning 404 for \"" << request.method_str() << " " << request.get_url_field(UF_PATH) << "\"");
  
  http_response response;
  response.add_header("Content-Type", "text/plain");
  response << "404 - not found\n";
  response << request.get_url_field(UF_PATH) << "\n";
  pipe.respond(response);
}

void server_term()
{
  for (auto mit = response_map.begin(); mit != response_map.end(); mit++) {
    for (auto it = mit->second.begin(); it != mit->second.end(); it++)
      delete it->second;
  }
}

void server_close_outgoing()
{
}


