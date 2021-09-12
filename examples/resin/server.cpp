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
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <aws/sqs/SQSClient.h>
#include <aws/sqs/model/SendMessageRequest.h>

using namespace Aws::SQS;
using namespace Aws::SQS::Model;
using namespace nlohmann;

namespace {

static void validate_command_file(const char* cmd_path)
{
  struct stat st;
  if (stat(cmd_path, &st) != 0)
  {
    if (errno == ENOENT)
    {
      if (mkfifo(cmd_path, 0666) != 0)
        do_error("mkfifo(\"" << &cmd_path[0] << "\", 0666");
      else
        return;
    }
    else
      do_error("stat(\"" << &cmd_path[0] << "\", &st)");
  }

  if (S_ISFIFO(st.st_mode))
    return;

  if (S_ISREG(st.st_mode))
  {
    if (unlink(cmd_path) != 0)
      do_error("unlink(\"" << cmd_path << "\")");
    validate_command_file(cmd_path);
    return;
  }
  else if (S_ISDIR(st.st_mode))
  {
    anon_log_error("\"" << &cmd_path[0] << "\" is a directory and must be manually deleted for this program to run");
    exit(1);
  }

  anon_log_error("\"" << &cmd_path[0] << "\" is an unknown file type and must be manually deleted for this program to run");
  exit(1);
}

}

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
      {"region", ec2i.default_region},
      {"instance_id", ec2i.instance_id},
      {"private_ipv4", ec2i.private_ipv4},
      {"public_ipv4", ec2i.public_ipv4},
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
  auto num_attempts = 0;
  while (true) {
    try {
      st = sync_teflon_app(ec2i);
      break;
    }
    catch(const std::exception& exc)
    {
      anon_log_error("server failed to start: " << exc.what());
      if (++num_attempts > 4)
        break;
      sleep(5);
    }
    catch(...)
    {
      anon_log_error("server failed to start");
      if (++num_attempts > 4)
        break;
      sleep(5);
    }
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
      {"region", ec2i.default_region},
      {"instance_id", ec2i.instance_id},
      {"private_ipv4", ec2i.private_ipv4},
      {"public_ipv4", ec2i.public_ipv4},
      {"tag", ud["sqs_tag"]}};

    SendMessageRequest req;
    req.WithQueueUrl(sqs_url)
      .WithMessageBody(msg.dump());
    auto outcome = sqs_client->SendMessage(req);
    if (!outcome.IsSuccess())
      anon_log_error("SQSClient.SendMessage failed: " << outcome.GetError().GetMessage());
  }
}
