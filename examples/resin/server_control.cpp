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
#include "server_control.h"
#include "http_parser.h"
#include "log.h"
#include <algorithm>
#include <map>
#include <aws/core/Aws.h>
#include <aws/sns/SNSClient.h>
#include <aws/sns/model/SubscribeRequest.h>

namespace {

bool process_control_message(const std::string& method, const std::string& url, const std::map<std::string, std::string>& headers, const std::vector<char>& body)
{
  anon_log("received control message - " << method << ":");
  anon_log(" url: " << url);
  anon_log(" headers: ");
  for (auto& h : headers)
    anon_log("  " << h.first << ": " << h.second);
  if (body.size())
    anon_log(" body: " << std::string(&body[0], body.size()));

  if (url == "/shut/down/now")
    return false;
  return true;
}

bool process_control_message(int fd)
{
  http_parser_settings settings;

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

  char buf[4096];
  size_t bsp = 0, bep = 0;
  while (!pcallback.message_complete)
  {
    if (bsp == bep) {
      auto bytes_read = read(fd, &buf[bep], sizeof(buf) - bep);
      if (bytes_read == -1) {
        anon_log("error reading from control socket: " << errno_string());
        return 1;
      }
      bep += bytes_read;
    }

    bsp += http_parser_execute(&parser, &settings, &buf[bsp], bep - bsp);

    if (pcallback.message_complete)
    {
      anon_log("received complete message, has_content_length: " << (pcallback.has_content_length ? "true" : "false"));
      std::vector<char> body;
      if (pcallback.has_content_length) {
        anon_log("pcallback.content_length: " << pcallback.content_length);
        body.resize(pcallback.content_length);
        uint64_t total_read = 0;
        while (total_read <  pcallback.content_length) {
          auto bytes_read = read(fd, &body[total_read], body.size() - total_read);
          if (bytes_read == -1) {
            anon_log("error reading from control socket: " << errno_string());
            return 1;
          }
          total_read += bytes_read;
          anon_log("read " << bytes_read << " bytes, total_read: " << total_read);
        }
      }
      anon_log("calling process_control_message");
      auto ret = process_control_message(http_method_str((enum http_method)pcallback.method), pcallback.url_str, pcallback.headers, body);
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
        cont = process_control_message(conn);
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
