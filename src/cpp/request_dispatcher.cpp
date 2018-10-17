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

#include "request_dispatcher.h"
#include "percent_codec.h"

response_strings response_strings::_singleton;
std::string response_strings::_empty = "";

response_strings::response_strings()
{
#define XX(num, name, string) _map[num] = #string;
  HTTP_STATUS_MAP(XX)
#undef XX
}

// path "specs" look like this:
//
//  some_host.com/some_partial_path/{thing_one}/some_more_partial_path/{thing_two}/maybe_even_more?queryName1&queryName2

request_helper request_mapping_helper(const std::string &path_spec)
{
  std::string path, query, headers;
  pcrecpp::RE split_at_q("([^?]*)(.*)");
  if (!split_at_q.FullMatch(path_spec, &path, &query))
  {
    anon_log_error("request_mapping failed, invalid path: " << path_spec);
    throw std::runtime_error("request_mapping failed, invalid path");
  }
  if (query.size() > 0)
  {
    if (!split_at_q.FullMatch(query.substr(1), &query, &headers))
    {
      anon_log_error("request_mapping failed, invalid path: " << path);
      throw std::runtime_error("request_mapping failed, invalid path");
    }
    if (headers.size() > 0)
      headers = headers.substr(1);
  }

  // firgure out how many {foo} statments are in the path
  pcrecpp::StringPiece input(path);
  int count = 0;
  pcrecpp::RE re1("({[^}]*})");
  while (re1.FindAndConsume(&input))
    ++count;

  // construct a regular expression for what we will be parsing
  // out of incomming uri's
  std::string newre_str = path;
  pcrecpp::RE("{[^}]*}").GlobalReplace("([^/]*)", &newre_str);

  request_helper h(newre_str, count);

  pcrecpp::RE split_at_and("([^&]+)");
  if (query.size() > 0)
  {
    pcrecpp::StringPiece input(query);
    std::string val;
    while (split_at_and.FindAndConsume(&input, &val))
      h.query_string_items.push_back(val);
  }

  if (headers.size() > 0)
  {
    pcrecpp::StringPiece input(headers);
    std::string val;
    while (split_at_and.FindAndConsume(&input, &val))
    {
      h.header_items.push_back(val);
    }
  }

  pcrecpp::RE split_at_var("([^?{]*)(.*)");
  if (!split_at_var.FullMatch(path_spec, &h.non_var))
    throw std::runtime_error("request_mapping failed, invalid path");

  return h;
}

std::pair<bool, std::vector<std::string>> extract_params(const request_helper &h, const http_request &request, const std::string &path, const std::string &query)
{
  auto ret = std::make_pair(false, std::vector<std::string>());

  std::vector<std::string> p(8);
  bool match = false;
  switch (h.num_path_substitutions)
  {
  case 0:
    match = h.path_re.FullMatch(path);
    break;
  case 1:
    match = h.path_re.FullMatch(path, &p[0]);
    break;
  case 2:
    match = h.path_re.FullMatch(path, &p[0], &p[1]);
    break;
  case 3:
    match = h.path_re.FullMatch(path, &p[0], &p[1], &p[2]);
    break;
  case 4:
    match = h.path_re.FullMatch(path, &p[0], &p[1], &p[2], &p[3]);
    break;
  case 5:
    match = h.path_re.FullMatch(path, &p[0], &p[1], &p[2], &p[3], &p[4]);
    break;
  case 6:
    match = h.path_re.FullMatch(path, &p[0], &p[1], &p[2], &p[3], &p[4], &p[5]);
    break;
  case 7:
    match = h.path_re.FullMatch(path, &p[0], &p[1], &p[2], &p[3], &p[4], &p[5], &p[6]);
    break;
  case 8:
    match = h.path_re.FullMatch(path, &p[0], &p[1], &p[2], &p[3], &p[4], &p[5], &p[6], &p[7]);
    break;
  default:
    break;
  }
  if (!match)
    return ret;
  for (auto &v : p)
  {
    if (v.size() == 0)
      break;
    ret.second.push_back(v);
  }

  if (h.query_string_items.size() > 0)
  {
    auto quer = percent_decode(query);
    for (auto &it : h.query_string_items)
    {
      size_t pos = 0;
      size_t skip_len = it.size() + 1; // for the "="
      while (true)
      {
        pos = quer.find(it + "=", pos);
        if (pos == 0 || (pos != std::string::npos && quer[pos - 1] == '&'))
          break;
        pos = quer.find("&", pos + skip_len);
        if (pos == std::string::npos)
          throw_request_error(HTTP_STATUS_BAD_REQUEST, "missing, required query field: " << it);
      }
      auto v = quer.substr(pos);
      ret.second.push_back(v.substr(0, v.find("&", 0)));
    }
  }

  if (h.header_items.size() > 0)
  {
    for (auto &it : h.header_items)
    {
      auto headers = request.headers;
      if (!headers.contains_header(it.c_str()))
        throw_request_error(HTTP_STATUS_BAD_REQUEST, "missing, required header: " << it);
      ret.second.push_back(headers.get_header(it.c_str()).str());
    }
  }

  ret.first = true;
  return ret;
}

void request_dispatcher::dispatch(http_server::pipe_t &pipe, const http_request &request, bool is_tls)
{
  request_wrap(pipe, [this, &pipe, &request, is_tls] {
    auto m = _map.find(request.method_str());
    if (m == _map.end())
      throw_request_error(HTTP_STATUS_METHOD_NOT_ALLOWED, "method: " << request.method_str());
    std::string path, query;
    if (!_split_at_q.FullMatch(request.get_url_field(UF_PATH), &path, &query))
      throw_request_error(400, "path split failed, invalid path: " << request.get_url_field(UF_PATH));
    auto e = m->second.upper_bound(path);
    if (e == m->second.begin())
      throw_request_error(HTTP_STATUS_NOT_FOUND, "resource: \"" << path << "\" not found");
    --e;
    for (auto &f : e->second)
    {
      if (f(pipe, request, is_tls, path, query))
        return;
    }
    throw_request_error(HTTP_STATUS_NOT_FOUND, "resource: \"" << path << "\" not found");
  });
}