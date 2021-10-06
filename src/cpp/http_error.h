/*
 Copyright (c) 2018 ANON authors, see AUTHORS file.
 
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
#include <map>
#include <string>
#include "nlohmann/json.hpp"

class response_strings
{
  std::map<int, std::string> _map;
  static response_strings _singleton;
  static std::string _empty;

public:
  response_strings();

  static const std::string &get_description(int code)
  {
    auto it = _singleton._map.find(code);
    return it != _singleton._map.end() ? it->second : _empty;
  }
};

struct request_error
{
  std::string code;
  std::string reason;
  std::string content_type;

  request_error(int c, const std::string &reason, const std::string& content_type = "text/plain")
      : reason(reason),
        content_type(content_type)
  {
    std::ostringstream code_str;
    code_str << std::to_string(c);
    auto &desc = response_strings::get_description(c);
    if (desc != "")
      code_str << " " << desc;
    code = code_str.str();
  }
};

template <typename T>
void throw_request_error_(int code, T err, const char* filename, int linenum)
{
  std::ostringstream format;
  err(format);
  #if defined(ANON_LOG_ALL_THROWS)
  Log::output(filename, linenum, [&](std::ostream &formatter) { formatter << format.str(); }, false);
  #endif
  format << "\n";
  auto msg = format.str();
  throw request_error(code, msg);
}

template <typename T>
void throw_request_js_error_(int code, const T&  js, const char* filename, int linenum)
{
  std::ostringstream format;
  format << js;
  #if defined(ANON_LOG_ALL_THROWS)
  Log::output(filename, linenum, [&](std::ostream &formatter) { formatter << format.str(); }, false);
  #endif
  auto msg = format.str();
  throw request_error(code, msg, "application/json");
}

inline void reply_back_error(const char* method_, int cors_enabled, const http_request &request,
      const char* msg, const char* response_code, const char* content_type, 
      const std::string& allowed_headers, http_server::pipe_t &pipe)
{
  http_response response;
  response.add_header("content-type", "text/plain");
  if (cors_enabled != 0) {
    std::string method = method_;
    bool chk = false;
    if (method == "OPTIONS")
      chk = true;
    else if (method == "GET")
      chk = cors_enabled & http_server::k_enable_cors_get;
    else if (method == "HEAD")
      chk = cors_enabled & http_server::k_enable_cors_head;
    else if (method == "POST")
      chk = cors_enabled & http_server::k_enable_cors_post;
    else if (method == "PUT")
      chk = cors_enabled & http_server::k_enable_cors_put;
    else if (method == "DELETE")
      chk = cors_enabled & http_server::k_enable_cors_put;
    if (chk) {
      if (request.headers.contains_header("origin"))
        response.add_header("access-control-allow-origin", request.headers.get_header("origin").str());
      response.add_header("access-control-allow-methods", method);
      response.add_header("access-control-allow-credentials", "true");
      response.add_header("access-control-max-age", "600");
      if (allowed_headers.size() > 0)
        response.add_header("access-control-allow-headers", allowed_headers);

    }
  }
  response.set_status_code(response_code);
  if (content_type != 0)
    response.add_header("content-type", content_type);
  response << msg << "\n";
  try {
    pipe.respond(response);
  }
  catch(...) {
    //anon_log("request_wrap caught exception trying to respond with error");
  }
}

/*
 * helper in reporting errors back to an http client caller.
 * Use it like this:
 * 
 *   throw_request_error(404, "I can't find: " << something << " because it doesn't exist");
 * 
 * This results in an http response code of 404, along with
 * the standard ("Not Found" for 404) message in the response
 * code, plus whatever you add with the second, streaming
 * description written into the body of the response.
 */
#define throw_request_error(_code, _body) throw_request_error_((int)_code, [&](std::ostream &formatter) { formatter << _body; }, __FILE__, __LINE__)

#define throw_request_js_error(_code, _js) throw_request_js_error_((int)_code, _js, __FILE__, __LINE__)

template <typename Fn>
void request_wrap(const char* method, int cors_enabled, const std::string& allow_headers_error, http_server::pipe_t &pipe, const http_request &request, Fn f)
{
  try
  {
    f();
  }
  catch (const request_error &e)
  {
    reply_back_error(method, cors_enabled, request, e.reason.c_str(), e.code.c_str(), e.content_type.c_str(), allow_headers_error, pipe);
  }
  catch (const nlohmann::json::exception& e)
  {
    reply_back_error(method, cors_enabled, request, e.what(), "400", "text/plain", allow_headers_error, pipe);
  }
  catch (const std::exception &e)
  {
    reply_back_error(method, cors_enabled, request, e.what(), "500", "text/plain", allow_headers_error, pipe);
  }
  catch (...)
  {
    reply_back_error(method, cors_enabled, request, "", "500", "text/plain", allow_headers_error, pipe);
  }
}
