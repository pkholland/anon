/*
 Copyright (c) 2018 Anon authors, see AUTHORS file.
 
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

#include "aws_http.h"
#include "dns_lookup.h"
#include "http_client.h"
#include <aws/core/http/standard/StandardHttpRequest.h>
#include <aws/core/http/standard/StandardHttpResponse.h>
#include <aws/core/http/Scheme.h>

using namespace Aws::Http;

namespace
{

class http_client : public HttpClient
{
  static std::string normalize(const std::string &path)
  {
    if (path.find("//") == 0)
      return path.substr(1);
    return path;
  }

public:
  http_client(const std::shared_ptr<aws_http_client_factory::epc_map> &maps, const std::shared_ptr<tls_context> &tls)
      : _maps(maps),
        _tls(tls)
  {
  }
  ~http_client() {}

  std::shared_ptr<endpoint_cluster> get_epc(const Aws::String &url) const
  {
    URI uri(url);
    auto key = uri.GetAuthority() + ":" + std::to_string(uri.GetPort());
    fiber_lock l(_maps->_mtx);
    auto &m = _maps->_epc_map;
    auto epc = m.find(key);
    if (epc != m.end())
      return epc->second;

    auto lookup = [uri, key]() -> std::unique_ptr<std::pair<int, std::vector<std::pair<int, sockaddr_in6>>>> {
#if ANON_LOG_NET_TRAFFIC > 1
      anon_log("looking up sockaddr for " << key);
#endif
      auto dnsl = dns_lookup::get_addrinfo(uri.GetAuthority().c_str(), uri.GetPort());

      std::unique_ptr<std::pair<int, std::vector<std::pair<int, sockaddr_in6>>>> ret(new std::pair<int, std::vector<std::pair<int, sockaddr_in6>>>());
      ret->first = dnsl.first;
      if (dnsl.first == 0)
      {
        for (auto a : dnsl.second)
        {
          a.sin6_port = htons(uri.GetPort());
#if ANON_LOG_NET_TRAFFIC > 1
          anon_log(" found: " << a);
#endif
          ret->second.push_back(std::make_pair(0 /*preference*/, a));
        }
      }
      return std::move(ret);
    };

    return m[key] = endpoint_cluster::create(lookup, uri.GetScheme() == Scheme::HTTPS, uri.GetAuthority().c_str(), _tls.get());
  }

  std::shared_ptr<HttpResponse> MakeRequest(HttpRequest &request,
                                            Aws::Utils::RateLimits::RateLimiterInterface *readLimiter,
                                            Aws::Utils::RateLimits::RateLimiterInterface *writeLimiter) const override
  {
    URI uri = request.GetUri();
    // anon_log("MakeRequest, url: " << uri.GetURIString());

    // auto start_time = cur_time();

    auto body = request.GetContentBody();
    std::vector<char> body_buff;
    if (body)
      body_buff = std::vector<char>((std::istreambuf_iterator<char>(*body)), std::istreambuf_iterator<char>());

    auto method = request.GetMethod();
    std::ostringstream str;
    str << HttpMethodMapper::GetNameForHttpMethod(method) << " " << normalize(uri.GetPath()) << " HTTP/1.1\r\n";
    auto headers = request.GetHeaders();
    for (auto &h : headers)
      str << h.first << ": " << h.second << "\r\n";
    if (body && !request.HasHeader(CONTENT_LENGTH_HEADER))
    {
      str << "transfer-encoding: identity\r\n";
      str << "content-length: " << body_buff.size() << "\r\n";
    }

    str << "\r\n";
    if (body)
      str.write(&body_buff[0], body_buff.size());
    auto message = str.str();

    std::shared_ptr<Standard::StandardHttpResponse> resp;
    auto read_body = method != HttpMethod::HTTP_HEAD;
    get_epc(uri.GetURIString())->with_connected_pipe([&request, &resp, &message, read_body](const pipe_t *pipe) {
      // anon_log("sending...\n\n" << message << "\n");
      pipe->write(message.c_str(), message.size());
      http_client_response re;
      re.parse(*pipe, read_body);
      resp = std::make_shared<Standard::StandardHttpResponse>(request);
      resp->SetResponseCode(static_cast<HttpResponseCode>(re.status_code));
      for (auto &h : re.headers.headers)
        resp->AddHeader(h.first.str(), h.second.str());
      for (auto &data : re.body)
        resp->GetResponseBody().write(&data[0], data.size());
    });

    // anon_log("time for transfer: " << cur_time() - start_time);

    return std::static_pointer_cast<HttpResponse>(resp);
  }

  std::shared_ptr<HttpResponse> MakeRequest(const std::shared_ptr<HttpRequest> &request,
                                            Aws::Utils::RateLimits::RateLimiterInterface *readLimiter,
                                            Aws::Utils::RateLimits::RateLimiterInterface *writeLimiter) const override
  {
    return MakeRequest(*request, readLimiter, writeLimiter);
  }

  std::shared_ptr<aws_http_client_factory::epc_map> _maps;
  std::shared_ptr<tls_context> _tls;
};

} // namespace

/////////////////////////////////////////////////////////////////////////////////////////////////

aws_http_client_factory::aws_http_client_factory()
    : _maps(std::make_shared<epc_map>()),
      _tls(std::make_shared<tls_context>(true /*client*/, nullptr /*verify_cert*/, "/etc/ssl/certs" /*verify_loc*/, nullptr, nullptr, 5))
{
}

std::shared_ptr<HttpClient> aws_http_client_factory::CreateHttpClient(const Aws::Client::ClientConfiguration &clientConfiguration) const
{
  return std::static_pointer_cast<HttpClient>(std::make_shared<http_client>(_maps, _tls));
}

std::shared_ptr<HttpRequest> aws_http_client_factory::CreateHttpRequest(const Aws::String &uri, HttpMethod method, const Aws::IOStreamFactory &streamFactory) const
{
  auto req = std::static_pointer_cast<HttpRequest>(std::make_shared<Standard::StandardHttpRequest>(uri, method));
  req->SetResponseStreamFactory(streamFactory);
  return req;
}

std::shared_ptr<HttpRequest> aws_http_client_factory::CreateHttpRequest(const URI &uri, HttpMethod method, const Aws::IOStreamFactory &streamFactory) const
{
  auto req = std::static_pointer_cast<HttpRequest>(std::make_shared<Standard::StandardHttpRequest>(uri, method));
  req->SetResponseStreamFactory(streamFactory);
  return req;
}
