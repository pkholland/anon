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
#include <sys/timerfd.h>
#include <signal.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
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

const int wait_secs = 10; // maximum time to wait for sqs messages
const int timeout_ms = wait_secs * 2 * 1000;
const int visibility_secs = 60;
const int visibility_refresh_secs = 30;

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
        auto bash_cmd = get_body(m);

        const auto max_retries = 2;

        const auto &att = m.GetAttributes();
        auto arc = att.find(Aws::SQS::Model::MessageSystemAttributeName::ApproximateReceiveCount);
        auto approx_receive_count = 1000;
        if (arc != att.end())
          approx_receive_count = std::stoull(arc->second.c_str());

        auto pid = fork();
        if (pid == -1)
          do_error("fork()");
        if (pid != 0)
        {
          // the calling parent
          int status;
          auto w = waitpid(pid, &status, 0);
          if (w == -1)
            do_error("waitpid(pid, &status, 0)");

          auto script_exited_zero = false;
          if (WIFEXITED(status))
          {
            anon_log("bash exited with exit code: " << WEXITSTATUS(status));
            script_exited_zero = WEXITSTATUS(status) == 0;
          }
          else if (WIFSIGNALED(status))
            anon_log("bash killed by signal: " << WTERMSIG(status));
          else if (WIFSTOPPED(status))
            anon_log("bash stopped by signal: " << WSTOPSIG(status));

          {
            // regardless of whether the bash command succeeded or not
            // we don't want to keep calling ChangeVisibilityStatus on this message
            std::unique_lock<std::mutex> l(keep_alive_mutex);
            keep_alive_set.erase(m.GetReceiptHandle());
          }

          // if the script succeeded, or if we have failed too many times,
          // delete the message from sqs
          if (script_exited_zero || approx_receive_count >= max_retries)
          {
            Aws::SQS::Model::DeleteMessageRequest req;
            req.WithQueueUrl(queue_url.c_str()).WithReceiptHandle(m.GetReceiptHandle());
            auto outcome = client.DeleteMessage(req);
            if (!outcome.IsSuccess())
              anon_log("DeleteMessage failed: " << outcome.GetError());
          }
        }
        else
        {
          // the child process
          auto bash_file_name = "/bin/bash";
          auto bash_fd = open(bash_file_name, O_RDONLY);
          if (bash_fd == -1)
            do_error("open(\"" << bash_file_name << "\", O_RDONLY)");

          const char *dash_c = "-c";
          const char *script = bash_cmd.c_str();
          const char *do_retry = approx_receive_count < max_retries ? "1" : "0";
          char *args[]{
              const_cast<char *>(bash_file_name),
              const_cast<char *>(dash_c),
              const_cast<char *>(script),
              const_cast<char *>(do_retry),
              0};

          fexecve(bash_fd, &args[0], environ);

          // if fexecve succeeded then we never get here.  So getting here is a failure,
          // but we are already in the child process at this point, so we do what we can
          // to signifify the error and then exit
          fprintf(stderr, "fexecve(%d, ...) failed with errno: %d - %s\n", bash_fd, errno, strerror(errno));
          exit(1);
        }
      }
    }
    else
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
