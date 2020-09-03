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

request_helper request_mapping_helper(const std::string &path_spec);

template <typename Fn, typename... Args>
void body_as_json(http_server::pipe_t &pipe, const http_request &request, Fn f, Args &&... args)
{
  // auto &h = request.headers;
  if (!request.has_content_length)
    throw_request_error(HTTP_STATUS_LENGTH_REQUIRED, "required Content-Length header is missing");
  auto clen = request.content_length;
  if (clen <= 2)
    throw_request_error(HTTP_STATUS_NOT_ACCEPTABLE, "Content-Length cannot be less than 2 (" << clen << ")");
  if (clen > 16384) {
    struct sockaddr_in6 addr6;
    socklen_t addr_len = sizeof(addr6);
    getpeername(pipe.get_fd(), (struct sockaddr *)&addr6, &addr_len);
    throw_request_error(HTTP_STATUS_PAYLOAD_TOO_LARGE, "Content-Length cannot exceed 16384 (" << clen << " - " << addr6 << ")");
  }
  std::vector<char> buff(clen);
  auto bytes_read = 0;
  while (bytes_read < clen)
    bytes_read += pipe.read(&buff[bytes_read], clen - bytes_read);
  nlohmann::json body = nlohmann::json::parse(buff.begin(), buff.end());
  f(pipe, request, std::forward<Args>(args)..., body);
}

inline void respond_options(http_server::pipe_t &pipe, const http_request &request)
{
  http_response response;
  response.add_header("allow", request.headers.get_header("access-control-request-method").str());
  response.set_status_code("204 No Content"); 
  pipe.respond(response);
}

#define n_pipe *(http_server::pipe_t *)0
#define n_request *(http_request *)0
#define n_json *(nlohmann::json *)0


std::pair<bool, std::vector<std::string>> extract_params(const request_helper &h, const http_request &request, const std::string &path, const std::string &query, bool is_options);

template <typename Fn>
auto get_map_responder(Fn f, const request_helper &h) -> decltype((void)(f(n_pipe, n_request)), std::function<bool(http_server::pipe_t &, const http_request &, bool, const std::string &, const std::string &s, bool)>())
{
  return [f, h](http_server::pipe_t &pipe, const http_request &request, bool, const std::string &path, const std::string &query, bool is_options) -> bool {
    auto params = extract_params(h, request, path, query, is_options);
    if (!params.first)
      return false;
    if (is_options)
      respond_options(pipe, request);
    else
      f(pipe, request);
    return true;
  };
}

template <typename Fn>
auto get_map_responder(Fn f, const request_helper &h) -> decltype((void)(f(n_pipe, n_request, std::string())), std::function<bool(http_server::pipe_t &, const http_request &, bool, const std::string &, const std::string &, bool)>())
{
  return [f, h](http_server::pipe_t &pipe, const http_request &request, bool, const std::string &path, const std::string &query, bool is_options) -> bool {
    auto params = extract_params(h, request, path, query, is_options);
    if (!params.first)
      return false;
    if (is_options)
      respond_options(pipe, request);
    else
      f(pipe, request, params.second[0]);
    return true;
  };
}

template <typename Fn>
auto get_map_responder(Fn f, const request_helper &h) -> decltype((void)(f(n_pipe, n_request, std::string(), std::string())), std::function<bool(http_server::pipe_t &, const http_request &, bool, const std::string &, const std::string &, bool)>())
{
  return [f, h](http_server::pipe_t &pipe, const http_request &request, bool, const std::string &path, const std::string &query,bool is_options) -> bool {
    auto params = extract_params(h, request, path, query, is_options);
    if (!params.first)
      return false;
    if (is_options)
      respond_options(pipe, request);
    else
      f(pipe, request, params.second[0], params.second[1]);
    return true;
  };
}

template <typename Fn>
auto get_map_responder(Fn f, const request_helper &h) -> decltype((void)(f(n_pipe, n_request, std::string(), std::string(), std::string())), std::function<bool(http_server::pipe_t &, const http_request &, bool, const std::string &, const std::string &, bool)>())
{
  return [f, h](http_server::pipe_t &pipe, const http_request &request, bool, const std::string &path, const std::string &query, bool is_options) -> bool {
    auto params = extract_params(h, request, path, query, is_options);
    if (!params.first)
      return false;
    if (is_options)
      respond_options(pipe, request);
    else
      f(pipe, request, params.second[0], params.second[1], params.second[2]);
    return true;
  };
}

template <typename Fn>
auto get_map_responder(Fn f, const request_helper &h) -> decltype((void)(f(n_pipe, n_request, std::string(), std::string(), std::string(), std::string())), std::function<bool(http_server::pipe_t &, const http_request &, bool, const std::string &, const std::string &, bool)>())
{
  return [f, h](http_server::pipe_t &pipe, const http_request &request, bool, const std::string &path, const std::string &query, bool is_options) -> bool {
    auto params = extract_params(h, request, path, query, is_options);
    if (!params.first)
      return false;
    if (is_options)
      respond_options(pipe, request);
    else
      f(pipe, request, params.second[0], params.second[1], params.second[2], params.second[3]);
    return true;
  };
}

template <typename Fn>
auto get_map_responder(Fn f, const request_helper &h) -> decltype((void)(f(n_pipe, n_request, std::string(), std::string(), std::string(), std::string(), std::string())), std::function<bool(http_server::pipe_t &, const http_request &, bool, const std::string &, const std::string &, bool)>())
{
  return [f, h](http_server::pipe_t &pipe, const http_request &request, bool, const std::string &path, const std::string &query, bool is_options) -> bool {
    auto params = extract_params(h, request, path, query, is_options);
    if (!params.first)
      return false;
    if (is_options)
      respond_options(pipe, request);
    else
      f(pipe, request, params.second[0], params.second[1], params.second[2], params.second[3], params.second[4]);
    return true;
  };
}

template <typename Fn>
auto get_map_responder_body(Fn f, const request_helper &h) -> decltype((void)(f(n_pipe, n_request, n_json)), std::function<bool(http_server::pipe_t &, const http_request &, bool, const std::string &, const std::string &, bool)>())
{
  return [f, h](http_server::pipe_t &pipe, const http_request &request, bool, const std::string &path, const std::string &query, bool is_options) -> bool {
    auto params = extract_params(h, request, path, query, is_options);
    if (!params.first)
      return false;
    if (is_options)
      respond_options(pipe, request);
    else
      body_as_json(pipe, request, f);
    return true;
  };
}

template <typename Fn>
auto get_map_responder_body(Fn f, const request_helper &h) -> decltype((void)(f(n_pipe, n_request, std::string(), n_json)), std::function<bool(http_server::pipe_t &, const http_request &, bool, const std::string &, const std::string &, bool)>())
{
  return [f, h](http_server::pipe_t &pipe, const http_request &request, bool, const std::string &path, const std::string &query, bool is_options) -> bool {
    auto params = extract_params(h, request, path, query, is_options);
    if (!params.first)
      return false;
    if (is_options)
      respond_options(pipe, request);
    else
      body_as_json(pipe, request, f, params.second[0]);
    return true;
  };
}

template <typename Fn>
auto get_map_responder_body(Fn f, const request_helper &h) -> decltype((void)(f(n_pipe, n_request, std::string(), std::string(), n_json)), std::function<bool(http_server::pipe_t &, const http_request &, bool, const std::string &, const std::string &, bool)>())
{
  return [f, h](http_server::pipe_t &pipe, const http_request &request, bool, const std::string &path, const std::string &query, bool is_options) -> bool {
    auto params = extract_params(h, request, path, query, is_options);
    if (!params.first)
      return false;
    if (is_options)
      respond_options(pipe, request);
    else
      body_as_json(pipe, request, f, params.second[0], params.second[1]);
    return true;
  };
}

template <typename Fn>
auto get_map_responder_body(Fn f, const request_helper &h) -> decltype((void)(f(n_pipe, n_request, std::string(), std::string(), std::string(), n_json)), std::function<bool(http_server::pipe_t &, const http_request &, bool, const std::string &, const std::string &, bool)>())
{
  return [f, h](http_server::pipe_t &pipe, const http_request &request, bool, const std::string &path, const std::string &query, bool is_options) -> bool {
    auto params = extract_params(h, request, path, query, is_options);
    if (!params.first)
      return false;
    if (is_options)
      respond_options(pipe, request);
    else
      body_as_json(pipe, request, f, params.second[0], params.second[1], params.second[2]);
    return true;
  };
}

template <typename Fn>
auto get_map_responder_body(Fn f, const request_helper &h) -> decltype((void)(f(n_pipe, n_request, std::string(), std::string(), std::string(), std::string(), n_json)), std::function<bool(http_server::pipe_t &, const http_request &, bool, const std::string &, const std::string &, bool)>())
{
  return [f, h](http_server::pipe_t &pipe, const http_request &request, bool, const std::string &path, const std::string &query, bool is_options) -> bool {
    auto params = extract_params(h, request, path, query, is_options);
    if (!params.first)
      return false;
    if (is_options)
      respond_options(pipe, request);
    else
      body_as_json(pipe, request, f, params.second[0], params.second[1], params.second[2], params.second[3]);
    return true;
  };
}

template <typename Fn>
auto get_map_responder_body(Fn f, const request_helper &h) -> decltype((void)(f(n_pipe, n_request, std::string(), std::string(), std::string(), std::string(), std::string(), n_json)), std::function<bool(http_server::pipe_t &, const http_request &, bool, const std::string &, const std::string &, bool)>())
{
  return [f, h](http_server::pipe_t &pipe, const http_request &request, bool, const std::string &path, const std::string &query, bool is_options) -> bool {
    auto params = extract_params(h, request, path, query, is_options);
    if (!params.first)
      return false;
    if (is_options)
      respond_options(pipe, request);
    else
      body_as_json(pipe, request, f, params.second[0], params.second[1], params.second[2], params.second[3], params.second[4]);
    return true;
  };
}


