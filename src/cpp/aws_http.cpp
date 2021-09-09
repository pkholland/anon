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

#ifndef ANON_AWS
#define ANON_AWS 1
#endif

#include "aws_http.h"
#include "dns_lookup.h"
#include "http_client.h"
#include <aws/core/http/standard/StandardHttpRequest.h>
#include <aws/core/http/standard/StandardHttpResponse.h>
#include <aws/core/http/Scheme.h>
#include <aws/core/utils/StringUtils.h>

using namespace Aws::Http;

namespace
{

bool _169_254_169_254_fails = false;

class http_client : public HttpClient
{
  static Aws::String normalize(const Aws::String &path)
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
    auto key = uri.GetAuthority() + ":" + Aws::Utils::StringUtils::to_string(uri.GetPort());
    fiber_lock l(_maps->_mtx);
    auto &m = _maps->_epc_map;
    auto epc = m.find(key);
    if (epc != m.end())
      return epc->second;

    auto newepc = endpoint_cluster::create(uri.GetAuthority().c_str(), uri.GetPort(),
                                             uri.GetScheme() == Scheme::HTTPS, _tls.get());
    if (uri.GetAuthority() != "169.254.169.254")
      newepc->disable_retries();
    //newepc->set_max_io_block_time(120);
    return m[key] = newepc;
  }

  void MakeRequest(const std::shared_ptr<Standard::StandardHttpResponse>& resp,
                                            const std::shared_ptr<HttpRequest> &request,
                                            URI uri,
                                            Aws::Utils::RateLimits::RateLimiterInterface *readLimiter,
                                            Aws::Utils::RateLimits::RateLimiterInterface *writeLimiter,
                                            int recursion) const
  {
    if (recursion > 4) {
      resp->SetResponseCode(HttpResponseCode::INTERNAL_SERVER_ERROR);
      return;
    }
    
    try {
      get_epc(uri.GetURIString())->with_connected_pipe([this, &request, &uri, &resp, readLimiter, writeLimiter, recursion](const pipe_t *pipe) -> bool {

        try {
          const auto& body = request->GetContentBody();
          auto method = request->GetMethod();
          *pipe << HttpMethodMapper::GetNameForHttpMethod(method) << " " << normalize(uri.GetPath())
                << uri.GetQueryString() << " HTTP/1.1\r\n";
          auto headers = request->GetHeaders();
          for (const auto &h : headers)
            *pipe << h.first << ": " << h.second << "\r\n";
          if (body && !request->HasHeader(CONTENT_LENGTH_HEADER))
          {
            *pipe << "transfer-encoding: identity\r\n";
            *pipe << "content-length: " << request->GetContentLength() << "\r\n";
          }
          *pipe << "\r\n";
          if (body)
            *pipe << body->rdbuf();

          auto read_body = method != HttpMethod::HTTP_HEAD;
          http_client_response re;
          re.parse(*pipe, read_body, false/*throw_on_server_error*/);
          if ((re.status_code == 301 || re.status_code == 302) && re.headers.contains_header("location")) {
            MakeRequest(resp, request, URI(re.headers.get_header("location").astr()), readLimiter, writeLimiter, recursion+1);
          }
          else {
            resp->SetResponseCode(static_cast<HttpResponseCode>(re.status_code));
            for (const auto &h : re.headers.headers)
              resp->AddHeader(h.first.astr(), h.second.astr());
            for (const auto &data : re.body)
              resp->GetResponseBody().write(&data[0], data.size());
          }
          return re.should_keep_alive;
        } catch(const fiber_io_error&) {
          resp->SetResponseCode(HttpResponseCode::NETWORK_READ_TIMEOUT);
        } catch(...) {
          resp->SetResponseCode(HttpResponseCode::NETWORK_READ_TIMEOUT);
        }
        return false;

      });
    } catch(...) {
      resp->SetResponseCode(HttpResponseCode::NETWORK_CONNECT_TIMEOUT);
    }

  }

  std::shared_ptr<HttpResponse> MakeRequest(const std::shared_ptr<HttpRequest>& request,
                Aws::Utils::RateLimits::RateLimiterInterface* readLimiter,
                Aws::Utils::RateLimits::RateLimiterInterface* writeLimiter) const
  {
    auto resp = std::make_shared<Standard::StandardHttpResponse>(request);
    if (_169_254_169_254_fails && (request->GetUri().GetURIString().find("http://169.254.169.254") == 0)) {
      resp->SetResponseCode(HttpResponseCode::BAD_GATEWAY);
    }
    else {
      try
      {
        MakeRequest(resp, request, request->GetUri(), readLimiter, writeLimiter, 0);
      }
      catch (const fiber_io_error &exc) {
        #if ANON_LOG_NET_TRAFFIC > 0
        anon_log("filber_io_error: " << exc.what());
        #endif
        if (request->GetUri().GetURIString().find("http://169.254.169.254") == 0) {
          _169_254_169_254_fails = true;
          resp->SetResponseCode(HttpResponseCode::BAD_GATEWAY);
        } else
          resp->SetResponseCode(HttpResponseCode::NETWORK_CONNECT_TIMEOUT); // set to "timeout" so the sdk performs a retry, see HttpResponseCode::IsRetryableHttpResponseCode
      }
      catch (const fiber_io_timeout_error &exc) {
        #if ANON_LOG_NET_TRAFFIC > 0
        anon_log("fiber_io_timeout_error: " << exc.what());
        #endif
        if (request->GetUri().GetURIString().find("http://169.254.169.254") == 0) {
          _169_254_169_254_fails = true;
          resp->SetResponseCode(HttpResponseCode::BAD_GATEWAY);
        } else
          resp->SetResponseCode(HttpResponseCode::NETWORK_CONNECT_TIMEOUT); // set to "timeout" so the sdk performs a retry, see HttpResponseCode::IsRetryableHttpResponseCode
      }
      #if ANON_LOG_NET_TRAFFIC > 0
      catch (const std::exception &exc)
      {
        anon_log("failure to write request: " << exc.what());
        resp->SetResponseCode(HttpResponseCode::REQUEST_NOT_MADE);
      }
      #endif
      catch (...)
      {
        #if ANON_LOG_NET_TRAFFIC > 0
        anon_log("unknown failure to write request");
        #endif
        resp->SetResponseCode(HttpResponseCode::REQUEST_NOT_MADE);
      }
    }

    return std::static_pointer_cast<HttpResponse>(resp);
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
