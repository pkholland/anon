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

  request_error(int c, const std::string &reason)
      : reason(reason)
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
void throw_request_error_(int code, T err)
{
  std::ostringstream format;
  err(format);
  format << "\n";
  auto msg = format.str();
  #if defined(ANON_LOG_ALL_THROWS)
  anon_log(msg);
  #endif
  throw request_error(code, msg);
}

inline void reply_back_error(const char* error_type, const char* msg, const char* response_code,
                    http_server::pipe_t &pipe)
{
  anon_log("request_wrap caught " << error_type << ": " << msg);
  http_response response;
  response.add_header("Content-Type", "text/plain");
  response.set_status_code(response_code);
  response << msg << "\n";
  try {
    pipe.respond(response);
  }
  catch(...) {
    anon_log("request_wrap caught exception trying to respond with error");
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
#define throw_request_error(_code, _body) throw_request_error_((int)_code, [&](std::ostream &formatter) { formatter << _body; })

template <typename Fn>
void request_wrap(http_server::pipe_t &pipe, Fn f)
{
  try
  {
    f();
  }
  catch (const request_error &e)
  {
    reply_back_error("request_error", e.reason.c_str(), e.code.c_str(), pipe);
  }
  catch (const nlohmann::json::exception& e)
  {
    reply_back_error("json exception", e.what(), "400", pipe);
  }
  catch (const std::exception &e)
  {
    reply_back_error("std::exception", e.what(), "500", pipe);
  }
  catch (...)
  {
    reply_back_error("unknown exception", "", "500", pipe);
  }
}
