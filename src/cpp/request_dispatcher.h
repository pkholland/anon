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
#include "nlohmann/json.hpp"
#include <pcrecpp.h>

struct request_helper
{
  pcrecpp::RE path_re;
  int num_path_substitutions;
  std::string non_var;
  std::vector<std::string> query_string_items;
  std::vector<std::string> header_items;

  request_helper(const pcrecpp::RE &path_re, int num_path_substitutions)
      : path_re(path_re),
        num_path_substitutions(num_path_substitutions)
  {
  }
};

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
  throw request_error(code, format.str());
}

#define throw_request_error(_code, _body) throw_request_error_(_code, [&](std::ostream &formatter) { formatter << _body; })

template <typename Fn>
void request_wrap(http_server::pipe_t &pipe, Fn f)
{
  try
  {
    f();
  }
  catch (const request_error &e)
  {
    http_response response;
    response.add_header("Content-Type", "text/plain");
    response.set_status_code(e.code);
    response << e.reason << "\n";
    pipe.respond(response);
  }
  catch (const std::exception &e)
  {
    http_response response;
    response.add_header("Content-Type", "text/plain");
    response.set_status_code("500");
    response << e.what() << "\n";
    pipe.respond(response);
  }
  catch (...)
  {
    http_response response;
    response.add_header("Content-Type", "text/plain");
    response.set_status_code("500");
    response << "caugh unknown exception\n";
    pipe.respond(response);
  }
}

request_helper request_mapping_helper(const std::string &path_spec);

template <typename Fn, typename... Args>
void body_as_json(http_server::pipe_t &pipe, const http_request &request, Fn f, Args &&... args)
{
  auto &h = request.headers;
  if (!h.contains_header("Content-Length"))
    throw request_error(HTTP_STATUS_LENGTH_REQUIRED, "required Content-Length header is missing");
  auto cl = h.get_header("Content-Length").str();
  auto clen = std::stoi(cl);
  if (clen <= 2)
    throw request_error(HTTP_STATUS_NOT_ACCEPTABLE, "Content-Length cannot be less than 2");
  if (clen > 16384)
    throw request_error(HTTP_STATUS_PAYLOAD_TOO_LARGE, "Content-Length cannot exceed clen");
  std::vector<char> buff(clen);
  pipe.read(&buff[0], clen);
  nlohmann::json body = nlohmann::json::parse(buff.begin(), buff.end());
  f(pipe, request, std::forward<Args>(args)..., body);
}

#define n_pipe *(http_server::pipe_t *)0
#define n_request *(http_request *)0

std::pair<bool, std::vector<std::string>> extract_params(const request_helper &h, const http_request &request, const std::string &path, const std::string &query);
template <typename Fn>
auto get_map_responder(Fn f, const request_helper &h) -> decltype((void)(f(n_pipe, n_request)), std::function<bool(http_server::pipe_t &, const http_request &, bool, const std::string &, const std::string &)>())
{
  return [f, h](http_server::pipe_t &pipe, const http_request &request, bool, const std::string &path, const std::string &query) -> bool {
    auto params = extract_params(h, request, path, query);
    if (!params.first)
      return false;
    f(pipe, request);
    return true;
  };
}

template <typename Fn>
auto get_map_responder(Fn f, const request_helper &h) -> decltype((void)(f(n_pipe, n_request, std::string())), std::function<bool(http_server::pipe_t &, const http_request &, bool, const std::string &, const std::string &)>())
{
  return [f, h](http_server::pipe_t &pipe, const http_request &request, bool, const std::string &path, const std::string &query) -> bool {
    auto params = extract_params(h, request, path, query);
    if (!params.first)
      return false;
    f(pipe, request, params.second[0]);
    return true;
  };
}

template <typename Fn>
auto get_map_responder(Fn f, const request_helper &h) -> decltype((void)(f(n_pipe, n_request, std::string(), std::string())), std::function<bool(http_server::pipe_t &, const http_request &, bool, const std::string &, const std::string &)>())
{
  return [f, h](http_server::pipe_t &pipe, const http_request &request, bool, const std::string &path, const std::string &query) -> bool {
    auto params = extract_params(h, request, path, query);
    if (!params.first)
      return false;
    f(pipe, request, params.second[0], params.second[1]);
    return true;
  };
}

template <typename Fn>
auto get_map_responder(Fn f, const request_helper &h) -> decltype((void)(f(n_pipe, n_request, std::string(), std::string(), std::string())), std::function<bool(http_server::pipe_t &, const http_request &, bool, const std::string &, const std::string &)>())
{
  return [f, h](http_server::pipe_t &pipe, const http_request &request, bool, const std::string &path, const std::string &query) -> bool {
    auto params = extract_params(h, request, path, query);
    if (!params.first)
      return false;
    f(pipe, request, params.second[0], params.second[1], params.second[2]);
    return true;
  };
}

template <typename Fn>
auto get_map_responder(Fn f, const request_helper &h) -> decltype((void)(f(n_pipe, n_request, std::string(), std::string(), std::string(), std::string())), std::function<bool(http_server::pipe_t &, const http_request &, bool, const std::string &, const std::string &)>())
{
  return [f, h](http_server::pipe_t &pipe, const http_request &request, bool, const std::string &path, const std::string &query) -> bool {
    auto params = extract_params(h, request, path, query);
    if (!params.first)
      return false;
    f(pipe, request, params.second[0], params.second[1], params.second[2], params.second[3]);
    return true;
  };
}

template <typename Fn>
auto get_map_responder(Fn f, const request_helper &h) -> decltype((void)(f(n_pipe, n_request, std::string(), std::string(), std::string(), std::string(), std::string())), std::function<bool(http_server::pipe_t &, const http_request &, bool, const std::string &, const std::string &)>())
{
  return [f, h](http_server::pipe_t &pipe, const http_request &request, bool, const std::string &path, const std::string &query) -> bool {
    auto params = extract_params(h, request, path, query);
    if (!params.first)
      return false;
    f(pipe, request, params.second[0], params.second[1], params.second[2], params.second[3], params.second[4]);
    return true;
  };
}

template <typename Fn>
auto get_map_responder_body(Fn f, const request_helper &h) -> decltype((void)(f(n_pipe, n_request, nlohmann::json())), std::function<bool(http_server::pipe_t &, const http_request &, bool, const std::string &, const std::string &)>())
{
  return [f, h](http_server::pipe_t &pipe, const http_request &request, bool, const std::string &path, const std::string &query) -> bool {
    auto params = extract_params(h, request, path, query);
    if (!params.first)
      return false;
    body_as_json(pipe, request, f);
    return true;
  };
}

template <typename Fn>
auto get_map_responder_body(Fn f, const request_helper &h) -> decltype((void)(f(n_pipe, n_request, std::string(), nlohmann::json())), std::function<bool(http_server::pipe_t &, const http_request &, bool, const std::string &, const std::string &)>())
{
  return [f, h](http_server::pipe_t &pipe, const http_request &request, bool, const std::string &path, const std::string &query) -> bool {
    auto params = extract_params(h, request, path, query);
    if (!params.first)
      return false;
    body_as_json(pipe, request, f, params.second[0]);
    return true;
  };
}

template <typename Fn>
auto get_map_responder_body(Fn f, const request_helper &h) -> decltype((void)(f(n_pipe, n_request, std::string(), std::string(), nlohmann::json())), std::function<bool(http_server::pipe_t &, const http_request &, bool, const std::string &, const std::string &)>())
{
  return [f, h](http_server::pipe_t &pipe, const http_request &request, bool, const std::string &path, const std::string &query) -> bool {
    auto params = extract_params(h, request, path, query);
    if (!params.first)
      return false;
    body_as_json(pipe, request, f, params.second[0], params.second[1]);
    return true;
  };
}

template <typename Fn>
auto get_map_responder_body(Fn f, const request_helper &h) -> decltype((void)(f(n_pipe, n_request, std::string(), std::string(), std::string(), nlohmann::json())), std::function<bool(http_server::pipe_t &, const http_request &, bool, const std::string &, const std::string &)>())
{
  return [f, h](http_server::pipe_t &pipe, const http_request &request, bool, const std::string &path, const std::string &query) -> bool {
    auto params = extract_params(h, request, path, query);
    if (!params.first)
      return false;
    body_as_json(pipe, request, f, params.second[0], params.second[1], params.second[2]);
    return true;
  };
}

template <typename Fn>
auto get_map_responder_body(Fn f, const request_helper &h) -> decltype((void)(f(n_pipe, n_request, std::string(), std::string(), std::string(), std::string(), nlohmann::json())), std::function<bool(http_server::pipe_t &, const http_request &, bool, const std::string &, const std::string &)>())
{
  return [f, h](http_server::pipe_t &pipe, const http_request &request, bool, const std::string &path, const std::string &query) -> bool {
    auto params = extract_params(h, request, path, query);
    if (!params.first)
      return false;
    body_as_json(pipe, request, f, params.second[0], params.second[1], params.second[2], params.second[3]);
    return true;
  };
}

template <typename Fn>
auto get_map_responder_body(Fn f, const request_helper &h) -> decltype((void)(f(n_pipe, n_request, std::string(), std::string(), std::string(), std::string(), std::string(), nlohmann::json())), std::function<bool(http_server::pipe_t &, const http_request &, bool, const std::string &, const std::string &)>())
{
  return [f, h](http_server::pipe_t &pipe, const http_request &request, bool, const std::string &path, const std::string &query) -> bool {
    auto params = extract_params(h, request, path, query);
    if (!params.first)
      return false;
    body_as_json(pipe, request, f, params.second[0], params.second[1], params.second[2], params.second[3], params.second[4]);
    return true;
  };
}

class request_dispatcher
{
  std::map<std::string, std::map<std::string, std::vector<std::function<bool(http_server::pipe_t &, const http_request &, bool, const std::string &, const std::string &)>>>> _map;
  pcrecpp::RE _split_at_q;
  pcrecpp::RE _split_at_var;
  std::string _root_path;

public:
  request_dispatcher(const std::string &root_path)
      : _split_at_q("([^?]*)(.*)"),
        _split_at_var("([^?{]*)(.*)"),
        _root_path(root_path)
  {
  }

  template <typename Fn>
  void request_mapping(const std::string &method, const std::string &path_spec, Fn f)
  {
    auto full_path_spec = _root_path + path_spec;
    std::string non_var, var;
    if (!_split_at_var.FullMatch(full_path_spec, &non_var, &var))
    {
      anon_log_error("path split failed, invalid path: " << full_path_spec);
      throw std::runtime_error("request_mapping failed, invalid path");
    }
    _map[method][non_var].push_back(get_map_responder(f, request_mapping_helper(full_path_spec)));
  }

  template <typename Fn>
  void request_mapping_body(const std::string &method, const std::string &path_spec, Fn f)
  {
    auto full_path_spec = _root_path + path_spec;
    std::string non_var, var;
    if (!_split_at_var.FullMatch(full_path_spec, &non_var, &var))
    {
      anon_log_error("path split failed, invalid path: " << full_path_spec);
      throw std::runtime_error("request_mapping failed, invalid path");
    }
    _map[method][non_var].push_back(get_map_responder_body(f, request_mapping_helper(full_path_spec)));
  }

  void dispatch(http_server::pipe_t &pipe, const http_request &request, bool is_tls);
};
