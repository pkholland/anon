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

#include "resin.h"
#include "sproc_mgr.h"
#include "log.h"
#include "sync_teflon_app.h"
#include "server_control.h"
#include <signal.h>
#include <aws/sqs/SQSClient.h>
#include <aws/sqs/model/SendMessageRequest.h>

using namespace Aws::SQS;
using namespace Aws::SQS::Model;
using namespace nlohmann;

void run_server(const ec2_info &ec2i)
{
  auto &ud = ec2i.user_data_js;
  if (ud.find("server_port") == ud.end() || ud.find("control_port") == ud.end()) {
    anon_log_error("user data missing required \"server_port\" and/or \"control_port\"");
    return;
  }
  int port = ud["server_port"];
  int cnt_port = ud["control_port"];

  std::vector<int> udp_ports;
  bool udp_is_ipv6 = false;
  if (ud.find("udp_ports") != ud.end()) {
    for (auto &p : ud["udp_ports"])
      udp_ports.push_back(p);
    if (ud.find("udp_is_ipv6") != ud.end())
      udp_is_ipv6 = ud["udp_is_ipv6"];
  }

  Aws::Client::ClientConfiguration config;
  std::shared_ptr<SQSClient> sqs_client;
  std::string sqs_url;
  if (ud.find("sqs_region") != ud.end() && ud.find("sqs_url") != ud.end() && ud.find("sqs_tag") != ud.end()) {
    std::string sqs_region = ud["sqs_region"];
    sqs_url = ud["sqs_url"];
    config.region = sqs_region;
    sqs_client = std::make_shared<SQSClient>(config);
    json msg = {
      {"operation", "start"},
      {"tag", ud["sqs_tag"]}};

    SendMessageRequest req;
    req.WithQueueUrl(sqs_url)
      .WithMessageBody(msg.dump());
    auto outcome = sqs_client->SendMessage(req);
    if (!outcome.IsSuccess())
      anon_throw(std::runtime_error, "SQSClient.SendMessage failed: " << outcome.GetError().GetMessage());
  }

  sigset_t sigs;
  sigemptyset(&sigs);
  sigaddset(&sigs, SIGPIPE);
  pthread_sigmask(SIG_BLOCK, &sigs, NULL);

  sproc_mgr_init(port, udp_ports, udp_is_ipv6);
  anon_log("resin bound to network port " << port);

  teflon_state st = teflon_server_failed;
  try {
    st = sync_teflon_app(ec2i);
  }
  catch(const std::exception& exc)
  {
    anon_log_error("server failed to start: " << exc.what());
  }
  catch(...)
  {
    anon_log_error("server failed to start");
  }

  if (st != teflon_server_running) {
    anon_log_error("cannot start teflon app - shutting down");
    sproc_mgr_term();
    return;
  }

  // stays here until we get an sns message
  // that causes us to shut down the server
  run_server_control(ec2i, cnt_port);

  stop_server();

  sproc_mgr_term();

  if (sqs_client) {
    json msg = {
      {"operation", "stop"},
      {"tag", ud["sqs_tag"]}};

    SendMessageRequest req;
    req.WithQueueUrl(sqs_url)
      .WithMessageBody(msg.dump());
    auto outcome = sqs_client->SendMessage(req);
    if (!outcome.IsSuccess())
      anon_log_error("SQSClient.SendMessage failed: " << outcome.GetError().GetMessage());
  }
}
