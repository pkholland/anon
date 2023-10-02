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
#include "time_utils.h"
#include "worker_message.pb.h"
#include "big_id_serial.h"
#include "big_id_crypto.h"
#include "tcp_utils.h"
#include <sys/timerfd.h>
#include <signal.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <netdb.h>
#include "nlohmann/json.hpp"
#include <aws/core/auth/AWSCredentialsProviderChain.h>
#include <aws/sqs/SQSClient.h>
#include <aws/sqs/model/ChangeMessageVisibilityBatchRequest.h>
#include <aws/sqs/model/ChangeMessageVisibilityBatchRequestEntry.h>
#include <aws/sqs/model/ReceiveMessageRequest.h>
#include <aws/sqs/model/DeleteMessageRequest.h>
#include <aws/dynamodb/DynamoDBClient.h>
#include <aws/dynamodb/model/GetItemRequest.h>
#include <aws/ec2/EC2Client.h>
#include <aws/ec2/model/DescribeInstancesRequest.h>
#include <aws/ec2/model/TerminateInstancesRequest.h>
#include <aws/ec2/model/StopInstancesRequest.h>

using namespace nlohmann;

namespace
{

Aws::String replace_all(Aws::String &s, const Aws::String &pat, const Aws::String &rep)
{
  size_t pos = 0;
  auto plen = pat.size();
  auto rlen = rep.size();
  while (true)
  {
    auto fpos = s.find(pat, pos);
    if (fpos == std::string::npos)
      break;
    s = s.replace(fpos, plen, rep);
    pos = fpos + rlen;
  }
  return s;
}

std::string get_body(const Aws::SQS::Model::Message &m)
{
  auto body = m.GetBody();
  body = replace_all(body, "&amp;", "&");
  body = replace_all(body, "&quot;", "\"");
  body = replace_all(body, "&apos;", "\'");
  body = replace_all(body, "&lt;", "<");
  return replace_all(body, "&gt;", ">").c_str();
}

//  if user data has:
//    {
//      "min_instance_url": <some dynamoDB url>,
//      "min_instance_region": optionally - a region (otherwise the current reation),
//      "min_instance_table_name": <name of a dynamoDB table>,
//      "min_instance_primary_key_name": <name of a key/record in that table>
//      "min_instance_primary_key_value": <value for min_instance_primary_key_name>
//      "min_instances": <name of a field in that record, of type "number">,
//      "min_instance_name": <value of the "Name" tag of this ec2 instance>
//    }
//  then this routine sees if there are more than "min_instances" ec2 instances
//  running that are tagged with Name: <min_instance_name>, and only shuts down
//  this instance if there are more than that number running.  This basically keeps
//  at least "min_instances" running all the time.
//
//  if one or more of these fields are not present then it shuts down this instance
bool should_shut_down(const ec2_info &ec2i)
{
  if (ec2i.user_data_js.find("min_instance_url") != ec2i.user_data_js.end())
  {
    Aws::Client::ClientConfiguration ddb_config;
    if (ec2i.user_data_js.find("min_instance_region") != ec2i.user_data_js.end()) {
      std::string reg = ec2i.user_data_js["min_instance_region"];
      ddb_config.region = reg.c_str();
    }
    else
      ddb_config.region = ec2i.default_region;
    Aws::DynamoDB::DynamoDBClient ddbc(ddb_config);

    Aws::DynamoDB::Model::AttributeValue primary_key;
    std::string mi_primary_key_value = ec2i.user_data_js["min_instance_primary_key_value"];
    primary_key.SetS(mi_primary_key_value.c_str());
    Aws::DynamoDB::Model::GetItemRequest req;
    std::string mi_table_name = ec2i.user_data_js["min_instance_table_name"];
    std::string mi_primary_key_name = ec2i.user_data_js["min_instance_primary_key_name"];
    req.WithTableName(mi_table_name.c_str())
        .AddKey(mi_primary_key_name.c_str(), primary_key);
    auto outcome = ddbc.GetItem(req);
    if (outcome.IsSuccess())
    {
      auto &map = outcome.GetResult().GetItem();
      auto it = map.find("min_instances");
      if (it != map.end())
      {
        auto min_instances = std::atoi(it->second.GetN().c_str());
        std::string instance_name = ec2i.user_data_js["min_instance_name"];

        Aws::EC2::Model::DescribeInstancesRequest request;
        Aws::EC2::Model::Filter filter1;
        filter1.SetName("tag:Name");
        filter1.AddValues(instance_name.c_str());
        request.AddFilters(filter1);
        Aws::EC2::Model::Filter filter2;
        filter2.SetName("instance-state-name");
        filter2.AddValues("running");
        request.AddFilters(filter2);

        bool done = false;
        int total_instances = 0;
        while (!done)
        {
          auto outcome = ec2i.ec2_client->DescribeInstances(request);
          if (outcome.IsSuccess())
          {
            const auto &reservations = outcome.GetResult().GetReservations();
            for (const auto &reservation : reservations)
            {
              const auto &instances = reservation.GetInstances();
              total_instances += instances.size();
              if (total_instances > min_instances)
                return true;
            }
            if (outcome.GetResult().GetNextToken().size() > 0)
              request.SetNextToken(outcome.GetResult().GetNextToken());
            else
              done = true;
          }
          else
            return true;
        }

        return total_instances > min_instances;
      }
    }
  }
  return true;
}

void do_shutdown(const ec2_info &ec2i, int attempts = 0)
{
  // we do certain kinds of debugging when this code isn't
  // running in EC2, and so there is nothing to "shut down".
  // If we are in that situation ec2.instance_id is set to
  // "instance_id" (which would never be a valid EC2 instance id)
  if (ec2i.instance_id == "instance_id") {
    return;
  }

  Aws::Client::ClientConfiguration ec2_config;
  ec2_config.region = ec2i.default_region;
  Aws::EC2::EC2Client ec2(ec2_config);

  Aws::EC2::Model::TerminateInstancesRequest request;
  request.AddInstanceIds(ec2i.instance_id);
  auto outcome = ec2.TerminateInstances(request);

  if (!outcome.IsSuccess() && attempts < 10)
  {

    anon_log("failed to stop instance " << ec2i.instance_id << ": " << outcome.GetError().GetMessage() << ", trying again");
    auto r = std::rand();
    auto milliseconds = (r % 5000);
    usleep(milliseconds * 1000);
    do_shutdown(ec2i, ++attempts);
  }
}

void start_done_action(const ec2_info &ec2i)
{
  auto done_action_it = ec2i.user_data_js.find("done_action");
  if (done_action_it != ec2i.user_data_js.end())
  {
    std::string done_action = *done_action_it;
    if (done_action == "terminate")
    {
      std::srand(std::time(0) + *(int *)ec2i.instance_id.c_str());
      do_shutdown(ec2i);
    }
    else if (done_action == "stop")
    {
      Aws::Client::ClientConfiguration ec2_config;
      ec2_config.region = ec2i.default_region;
      Aws::EC2::EC2Client ec2(ec2_config);
      Aws::EC2::Model::StopInstancesRequest request;
      request.AddInstanceIds(ec2i.instance_id.c_str());
      auto outcome = ec2.StopInstances(request);
      if (!outcome.IsSuccess())
        anon_log("failed to stop instance " << ec2i.instance_id << ": " << outcome.GetError().GetMessage());
      else
        anon_log("instance succesfully stopped");
    }
  }
  else
    anon_log("no done action specified, leaving instance running");
}

std::pair<bool, std::string> exe_cmd(const std::string &cmd)
{
  std::string out;
  auto f = popen(cmd.c_str(), "r");
  if (f) {
    char buff[1024];
    while (true) {
      auto bytes = fread(&buff[0], 1, sizeof(buff), f);
      if (bytes <= 0)
        break;
      out += std::string(&buff[0], bytes);
    }
    auto exit_code = pclose(f);
    return std::make_pair(exit_code == 0, out);
  }
  return std::make_pair(false, std::string());
}

const int wait_secs = 10; // maximum time to wait for sqs messages
const int default_idle_time = wait_secs;
const int timeout_ms = wait_secs * 2 * 1000;
const int visibility_secs = 60;
const int visibility_refresh_secs = 30;
int udp_sock = -1;
sockaddr_in6 udp_addr;
size_t udp_addr_sz;
std::string worker_id;

void init_udp_socket(const std::string& host, int port)
{
  anon_log("starting worker with upd_host: " << host << ", port: " << port);
  struct addrinfo hints = {};
  hints.ai_family = AF_UNSPEC; // use IPv4 or IPv6, whichever
  hints.ai_socktype = SOCK_STREAM;
  char portString[8];
  sprintf(&portString[0], "%d", port);
  struct addrinfo *result;
  auto err = getaddrinfo(host.c_str(), &portString[0], &hints, &result);
  if (err == 0)
  {
    auto ipv6 = result->ai_addr->sa_family == AF_INET6;
    udp_addr_sz = ipv6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
    memcpy(&udp_addr, result->ai_addr, udp_addr_sz);
    freeaddrinfo(result);

    udp_sock = socket(ipv6 ? AF_INET6 : AF_INET, SOCK_DGRAM, 0);
    if (udp_sock == -1)
      do_error("socket(AF_INET6, SOCK_DGRAM, 0)");

    // bind to any address that will route to this machine
    struct sockaddr_in6 addr = {0};
    socklen_t sz;
    if (ipv6)
    {
      addr.sin6_family = AF_INET6;
      addr.sin6_port = 0;
      addr.sin6_addr = in6addr_any;
      sz = sizeof(sockaddr_in6);
    }
    else
    {
      auto addr4 = (struct sockaddr_in*)&addr;
      addr4->sin_family = AF_INET;
      addr4->sin_port = 0;
      addr4->sin_addr.s_addr = INADDR_ANY;
      sz = sizeof(sockaddr_in);
    }
    if (bind(udp_sock, (struct sockaddr *)&addr, sz) != 0)
    {
      close(udp_sock);
      udp_sock = -1;
      do_error("bind(<AF_INET/6 SOCK_DGRAM socket>, <" << 0 << ", in6addr_any/INADDR_ANY>, sizeof(...))");
    }
    else {
      worker_id = toHexString(small_rand_id());
      anon_log("worker host bound to local addr: " << addr << ", worker_id: " << worker_id);
    }
  }
}

void send_udp_message(const resin_worker::Message& msg)
{
  std::string bytes;
  if (msg.SerializeToString(&bytes)) {
    if (::sendto(udp_sock, bytes.c_str(), bytes.size(), 0, (sockaddr*)&udp_addr, udp_addr_sz) == -1) {
      anon_log("sendto failed with errno: " << errno_string());
    }
  }
  else {
    anon_log("msg.SerializeToString failed");
  }
}

} // namespace

void run_worker(const ec2_info &ec2i)
{
  Aws::Client::ClientConfiguration config;
  auto queue_region_it = ec2i.user_data_js.find("task_queue_region");
  if (queue_region_it != ec2i.user_data_js.end()) {
    config.region = *queue_region_it;
  } else
    config.region = ec2i.default_region;
  config.httpRequestTimeoutMs = config.requestTimeoutMs = config.connectTimeoutMs = timeout_ms;

  std::string queue_url = ec2i.user_data_js["task_queue_url"];
  anon_log("reading tasks from: " << queue_url);

  auto idle_time = default_idle_time;
  auto idle_time_it = ec2i.user_data_js.find("idle_time_in_seconds");
  if (idle_time_it != ec2i.user_data_js.end() && idle_time_it->is_number()) {
    idle_time = *idle_time_it;
  }

  auto udp_host_it = ec2i.user_data_js.find("status_udp_host");
  auto udp_port_it = ec2i.user_data_js.find("status_udp_port");
  if (udp_host_it != ec2i.user_data_js.end() && udp_host_it->is_string()
    && udp_port_it != ec2i.user_data_js.end() && udp_port_it->is_number()) {

    init_udp_socket(*udp_host_it, *udp_port_it);
    if (udp_sock != -1) {
      resin_worker::Message msg;
      msg.set_message_type(resin_worker::Message_MessageType::Message_MessageType_WORKER_STATUS);
      auto ws = msg.mutable_worker_status();
      ws->set_cpu_count(std::thread::hardware_concurrency());
      ws->set_worker_id(worker_id);
      anon_log("sending worker startup message");
      send_udp_message(msg);
    }
  }

  Aws::SQS::SQSClient client(config);
  std::mutex keep_alive_mutex;
  std::map<Aws::String, Aws::String> keep_alive_set;
  bool stop = false;
  auto timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  struct itimerspec t_spec = {0};
  t_spec.it_value = cur_time() + visibility_refresh_secs;
  timerfd_settime(timerfd, TFD_TIMER_ABSTIME, &t_spec, 0);

  std::thread keep_alive_thread([&client, &keep_alive_mutex, &keep_alive_set,
                                 &stop, timerfd, queue_url] {
    while (true)
    {
      struct pollfd pfd;
      pfd.fd = timerfd;
      pfd.events = POLLIN;
      auto ret = poll(&pfd, 1, (visibility_refresh_secs + 1) * 1000);

      if (ret < 0)
        anon_log("poll failed: " << errno_string());
      else if (ret > 0) {
        // clear the data written to this pipe
        uint64_t unused;
        if (read(timerfd, &unused, sizeof(unused))) {}
      }

      std::unique_lock<std::mutex> l(keep_alive_mutex);
      if (stop)
        break;

      auto num_messages = keep_alive_set.size();
      if (num_messages > 0)
      {
        Aws::SQS::Model::ChangeMessageVisibilityBatchRequest req;
        Aws::Vector<Aws::Vector<Aws::SQS::Model::ChangeMessageVisibilityBatchRequestEntry>> entries_v;
        int index = 0;
        for (auto it : keep_alive_set)
        {
          Aws::SQS::Model::ChangeMessageVisibilityBatchRequestEntry ent;
          std::ostringstream str;
          if (index % 10 == 0)
            entries_v.push_back(Aws::Vector<Aws::SQS::Model::ChangeMessageVisibilityBatchRequestEntry>());
          str << "message_" << ++index;
          ent.WithReceiptHandle(it.first).WithVisibilityTimeout(visibility_secs).WithId(str.str().c_str());
          entries_v.back().push_back(ent);
        }
        l.unlock();
        for (auto &entries : entries_v)
        {
          req.WithQueueUrl(queue_url.c_str()).WithEntries(entries);
          auto outcome = client.ChangeMessageVisibilityBatch(req);
          if (!outcome.IsSuccess())
            anon_log("ChangeMessageVisibilityBatch failed: " << outcome.GetError());
        }
      }

      struct itimerspec t_spec = {0};
      t_spec.it_value = cur_time() + visibility_refresh_secs;
      timerfd_settime(timerfd, TFD_TIMER_ABSTIME, &t_spec, 0);
    }
  });

  auto last_message_time = cur_time();

  while (true)
  {
    Aws::SQS::Model::ReceiveMessageRequest req;
    req.WithQueueUrl(queue_url.c_str()).WithMaxNumberOfMessages(1).WithWaitTimeSeconds(wait_secs);
    Aws::Vector<Aws::SQS::Model::QueueAttributeName> att;
    att.push_back(Aws::SQS::Model::QueueAttributeName::All);
    req.WithAttributeNames(std::move(att));

    auto outcome = client.ReceiveMessage(req);
    if (!outcome.IsSuccess())
    {
      anon_log("ReceiveMessage failed: " << outcome.GetError());
      break;
    }
    auto &messages = outcome.GetResult().GetMessages();
    if (messages.size() > 0)
    {
      {
        std::unique_lock<std::mutex> l(keep_alive_mutex);
        for (auto &m : messages)
          keep_alive_set[m.GetReceiptHandle()] = m.GetMessageId();
      }
      struct itimerspec t_spec = {0};
      t_spec.it_value = cur_time();
      timerfd_settime(timerfd, TFD_TIMER_ABSTIME, &t_spec, 0);

      for (auto &m : messages)
      {
        auto valid_message = true;
        auto cmd = get_body(m);
        std::string bash_cmd;
        std::string task_id;
        try {
          auto js = json::parse(cmd);
          auto type_it = js.find("type");
          if (type_it == js.end() || !type_it->is_string()) {
            anon_log("\"type\" field is missing/incorrect");
            valid_message = false;
          }
          else {
            std::string type = *type_it;
            if (type != "bash_command") {
              anon_log("unsupported command type: " << type);
              valid_message = false;
            }
            auto cmd_it = js.find("command");
            if (cmd_it == js.end() || !cmd_it->is_string()) {
              anon_log("\"command\" field is missing/incorrect");
              valid_message = false;
            }
            else {
              bash_cmd = *cmd_it;
              auto task_id_it = js.find("task_id");
              if (task_id_it != js.end() && task_id_it->is_string()) {
                task_id = *task_id_it;
              }
            }
          }
        }
        catch(...) {
          anon_log("command message could not be parsed as json: " << cmd);
          valid_message = false;
        }

        if (!valid_message) {
          Aws::SQS::Model::DeleteMessageRequest req;
          req.WithQueueUrl(queue_url.c_str()).WithReceiptHandle(m.GetReceiptHandle());
          auto outcome = client.DeleteMessage(req);
          if (!outcome.IsSuccess())
            anon_log("DeleteMessage failed: " << outcome.GetError());
          continue;
        }

        const auto max_retries = 2;

        const auto &att = m.GetAttributes();
        auto arc = att.find(Aws::SQS::Model::MessageSystemAttributeName::ApproximateReceiveCount);
        auto approx_receive_count = 1000;
        if (arc != att.end()) {
          approx_receive_count = std::stoull(arc->second.c_str());
        }

        if (udp_sock != -1 && !task_id.empty()) {
          resin_worker::Message msg;
          msg.set_message_type(resin_worker::Message_MessageType::Message_MessageType_TASK_STATUS);
          auto ts = msg.mutable_task_status();
          ts->set_worker_id(worker_id);
          ts->set_task_id(task_id);
          ts->set_completed(0.0f);
          ts->set_complete(false);
          anon_log("sending task start message");
          send_udp_message(msg);
        }

        auto start_time = cur_time();

        auto out = exe_cmd(bash_cmd);

        if (out.first || approx_receive_count >= max_retries)
        {
          if (udp_sock != -1 && !task_id.empty()) {
            resin_worker::Message msg;
            msg.set_message_type(resin_worker::Message_MessageType::Message_MessageType_TASK_STATUS);
            auto ts = msg.mutable_task_status();
            ts->set_worker_id(worker_id);
            ts->set_task_id(task_id);
            ts->set_completed(1.0f);
            ts->set_complete(true);
            ts->set_success(out.first);
            ts->set_duration(to_seconds(cur_time() - start_time));
            ts->set_message(out.second);
            anon_log("sending task done message");
            send_udp_message(msg);
          }

          Aws::SQS::Model::DeleteMessageRequest req;
          req.WithQueueUrl(queue_url.c_str()).WithReceiptHandle(m.GetReceiptHandle());
          auto outcome = client.DeleteMessage(req);
          if (!outcome.IsSuccess())
            anon_log("DeleteMessage failed: " << outcome.GetError());
        }
      }
      last_message_time = cur_time();
    }
    else if (to_seconds(cur_time() - last_message_time) >= idle_time)
    {
      anon_log("no tasks after wating " << wait_secs << " seconds");
      if (should_shut_down(ec2i))
      {
        anon_log("no reason to keep running, executing done_action");
        start_done_action(ec2i);
        break;
      }
    }
  }

    if (udp_sock != -1) {
      resin_worker::Message msg;
      msg.set_message_type(resin_worker::Message_MessageType::Message_MessageType_WORKER_STATUS);
      auto ws = msg.mutable_worker_status();
      ws->set_cpu_count(0);
      ws->set_worker_id(worker_id);
      anon_log("sending worker shutdown message");
      send_udp_message(msg);
    }

  // tell the keep_alive thread to wake up and exit.
  // then wait for it to have fully exited.
  {
    std::unique_lock<std::mutex> l(keep_alive_mutex);
    stop = true;
    struct itimerspec t_spec = {0};
    t_spec.it_value = cur_time();
    timerfd_settime(timerfd, TFD_TIMER_ABSTIME, &t_spec, 0);
  }
  keep_alive_thread.join();

  close(timerfd);
}
