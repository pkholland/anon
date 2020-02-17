/*
 Copyright (c) 2020 ANON authors, see AUTHORS file.
 
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

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include "server_control.h"
#include "http_parser.h"
#include "log.h"
#include <algorithm>
#include <map>
#include <aws/core/Aws.h>
#include <aws/sns/SNSClient.h>
#include <aws/sns/model/SubscribeRequest.h>
#include <aws/sns/model/ConfirmSubscriptionRequest.h>
#include "sync_teflon_app.h"

using namespace nlohmann;

namespace {

bool subscription_confirmed = false;

bool process_control_message(const ec2_info& ec2i, const std::string& method, const std::string& url, const std::map<std::string, std::string>& headers, const std::vector<char>& body)
{
  if (true) {
    anon_log("received control message - " << method << ":");
    anon_log(" url: " << url);
    anon_log(" headers: ");
    for (auto& h : headers)
      anon_log("  " << h.first << ": " << h.second);
    if (body.size())
      anon_log(" body: " << std::string(&body[0], body.size()));
  }

  if (subscription_confirmed && url == "/sns") {
    json js = json::parse(body);
    if (js.find("Type") == js.end()
      || js.find("Token") == js.end()) {
      anon_log("sys confirmation message arived without needed fields in body");
    }
    std::string type = js["Type"];
    if (type == "SubscriptionConfirmation") {
      std::string token = js["Token"];

      Aws::Client::ClientConfiguration sns_config;
      if (ec2i.user_data_js.find("sns_region") != ec2i.user_data_js.end())
        sns_config.region = ec2i.user_data_js["sns_region"];
      else
        sns_config.region = ec2i.default_region;
      
      Aws::SNS::SNSClient client(sns_config);

      Aws::SNS::Model::ConfirmSubscriptionRequest req;
      std::string sns_topic = ec2i.user_data_js["sns_topic"];
      req.WithTopicArn(sns_topic.c_str()).
        WithToken(token.c_str()).WithAuthenticateOnUnsubscribe("true");

      auto outcome = client.ConfirmSubscription(req);
      if (outcome.IsSuccess()) {
        anon_log("ConfirmSubscription succeeded");
      } else {
        anon_log ("ConfirmSubscription failed: " << outcome.GetError());
      }
      subscription_confirmed = true;
    }
    else if (type == "Notification") {
      if (sync_teflon_app(ec2i) == teflon_shut_down)
        return false;
    }

  }
  return true;
}

ssize_t timed_read(int fd, void* buff, size_t nbytes)
{
  struct pollfd pfd;
  pfd.fd = fd;
  pfd.events = POLLIN;
  auto ret = poll(&pfd, 1, 500);
  if (ret == 0) {
    anon_log("control message taking too long to fully arive");
    errno = EINTR;
    return -1;
  }
  if (ret < 0)
    return ret;
  return read(fd, buff, nbytes);
}

// returns:
// true  -> "keep running the loop waiting for messages"
// false -> "shut down the app"
bool process_control_message(const ec2_info& ec2i, int fd)
{
  http_parser_settings settings;
  const double max_message_time = 2.0;

  struct pc
  {
    pc()
    {
      init();
    }

    void init()
    {
      message_complete = false;
      to_lower = false;
      memset(&p_url, 0, sizeof(p_url));
      url_str = "";
      cur_header_field = "";
    }

    http_parser_url p_url;
    bool message_complete;
    std::string url_str;
    std::map<std::string, std::string> headers;
    std::string cur_header_field;
    std::string cur_header_field_lower;
    unsigned short http_major;
    unsigned short http_minor;
    unsigned int method;
    bool has_content_length;
    bool to_lower;
    uint64_t content_length;

  };
  pc pcallback;

  settings.on_message_begin = [](http_parser *p) -> int {
    return 0;
  };

  settings.on_url = [](http_parser *p, const char *at, size_t length) -> int {
    pc *c = (pc *)p->data;
    c->url_str += std::string(at, length);
    http_parser_parse_url(at, length, true, &c->p_url);
    return 0;
  };

  settings.on_status = [](http_parser *p, const char *at, size_t length) -> int {
    return 0;
  };

  settings.on_header_field = [](http_parser *p, const char *at, size_t length) -> int {
    pc *c = (pc *)p->data;
    c->cur_header_field += std::string(at, length);
    c->to_lower = false;
    return 0;
  };

  settings.on_header_value = [](http_parser *p, const char *at, size_t length) -> int {
    pc *c = (pc *)p->data;
    if (!c->to_lower) {
      std::vector<char> buff(c->cur_header_field.size());
      memcpy(&buff[0], c->cur_header_field.c_str(), c->cur_header_field.size());
      std::transform(&buff[0], &buff[0] + c->cur_header_field.size(), &buff[0],
        [](char c){ return std::tolower(c); });
      c->cur_header_field_lower = std::string(&buff[0], buff.size());
      c->to_lower = true;
      c->cur_header_field = "";
    }
    c->headers[c->cur_header_field_lower] += std::string(at, length);
    return 0;
  };

  settings.on_headers_complete = [](http_parser *p) -> int {
    pc *c = (pc *)p->data;
    c->http_major = p->http_major;
    c->http_minor = p->http_minor;
    c->method = p->method;
    c->has_content_length = (p->flags & F_CONTENTLENGTH) != 0;
    c->content_length = p->content_length;

    // note, see code in http_parser.c, returning 1 causes the
    // parser to skip attempting to read the body, which is
    // what we want.
    return 1;
  };

  settings.on_body = [](http_parser *p, const char *at, size_t length) -> int {
    anon_log("parser on_body");
    return 1;
  };

  settings.on_message_complete = [](http_parser *p) -> int {
    pc *c = (pc *)p->data;
    c->message_complete = true;
    anon_log("parser on_message_complete");
    return 0;
  };

  http_parser parser;
  parser.data = &pcallback;
  http_parser_init(&parser, HTTP_REQUEST);

  auto start_time = std::chrono::system_clock::now();

  char buf[4096];
  size_t bsp = 0, bep = 0;
  while (!pcallback.message_complete)
  {
    auto cur_time = std::chrono::system_clock::now();
    std::chrono::duration<double> dur = cur_time - start_time;
    if (dur.count() > max_message_time) {
      anon_log("control message taking too long to fully arive");
      return true;
    }
    if (bsp == bep) {
      auto bytes_read = timed_read(fd, &buf[bep], sizeof(buf) - bep);
      if (bytes_read <= 0) {
        anon_log("error reading from control socket: " << errno_string());
        return true;
      }
      bep += bytes_read;
    }

    bsp += http_parser_execute(&parser, &settings, &buf[bsp], bep - bsp);

    if (pcallback.message_complete)
    {
      std::vector<char> body;
      if (pcallback.has_content_length) {
        body.resize(pcallback.content_length);
        memcpy(&body[0], &buf[bsp], bep - bsp);
        uint64_t total_read = bep - bsp;
        while (total_read <  pcallback.content_length) {
          std::chrono::duration<double> dur = cur_time - start_time;
          if (dur.count() > max_message_time) {
            anon_log("control message taking too long to fully arive");
            return true;
          }
          auto bytes_read = timed_read(fd, &body[total_read], body.size() - total_read);
          if (bytes_read == -1) {
            anon_log("error reading from control socket: " << errno_string());
            return true;
          }
          total_read += bytes_read;
        }
      }
      auto ret = process_control_message(ec2i, http_method_str((enum http_method)pcallback.method), pcallback.url_str, pcallback.headers, body);
      auto reply = "HTTP/1.1 200 OK\r\n";
      write(fd, reply, strlen(reply));
      return ret;
    }
    else if (bsp == sizeof(buf))
    {
      anon_log("invalid control message, headers larger than " << sizeof(buf[4096]) << " bytes");
      return true;
    }
    
  }

  return true;
}

}

void run_server_control(const ec2_info& ec2i, int port)
{
  auto listen_sock = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
  if (listen_sock == -1)
    do_error("socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK, IPPROTO_TCP)");

  // bind to any address that will route to this machine
  struct sockaddr_in6 addr = {0};

  addr.sin6_family = AF_INET6;
  addr.sin6_port = htons(port);
  addr.sin6_addr = in6addr_any;
  if (bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0)
  {
    close(listen_sock);
    do_error("bind(" << listen_sock << ", <port: " << port << ", in6addr_any>, sizeof(addr))");
  }

  if (listen(listen_sock, SOMAXCONN) != 0)
  {
    close(listen_sock);
    do_error("listen(" << listen_sock << ", SOMAXCONN)");
  }

  anon_log("listening to control port " << port << " with fd " << listen_sock);

  if (ec2i.user_data_js.find("sns_topic") != ec2i.user_data_js.end()) {
    Aws::Client::ClientConfiguration sns_config;
    if (ec2i.user_data_js.find("sns_region") != ec2i.user_data_js.end())
      sns_config.region = ec2i.user_data_js["sns_region"];
    else
      sns_config.region = ec2i.default_region;
    
    Aws::SNS::SNSClient client(sns_config);

    Aws::SNS::Model::SubscribeRequest req;
    std::string sns_topic = ec2i.user_data_js["sns_topic"];
    std::string endpoint = "http://";
    endpoint += ec2i.public_ipv4 + ":" + std::to_string(port) + "/sns";
    req.WithTopicArn(sns_topic.c_str()).WithProtocol("http")
      .WithEndpoint(endpoint.c_str());

    anon_log("subscribing to topic: " << sns_topic << ", endpoint: " << endpoint << ", region: " << sns_config.region);

    auto outcome = client.Subscribe(req);
    if (!outcome.IsSuccess())
      anon_log("sns subscribe failed: " << outcome.GetError());
  }

  auto cont = true;
  while (cont)
  {
    struct sockaddr_in6 addr;
    socklen_t addr_len = sizeof(addr);
    auto conn = accept(listen_sock, (struct sockaddr *)&addr, &addr_len);
    if (conn == -1)
    {
      anon_log("accept failed: " << errno_string());
    }
    else
    {
      try
      {
        cont = process_control_message(ec2i, conn);
      }
      catch (const std::exception &exc)
      {
        anon_log("processing control message failed: " << exc.what());
      }
      catch (...)
      {
        anon_log("procesing control message failed");
      }
      close(conn);
    }
  }
}
