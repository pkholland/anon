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

namespace
{

std::string replace_all(std::string &s, const std::string &pat, const std::string &rep)
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
  return replace_all(body, "&gt;", ">");
}

bool should_shut_down(const ec2_info &ec2i)
{
  if (ec2i.user_data_js.find("min_instance_url") != ec2i.user_data_js.end())
  {
    Aws::Client::ClientConfiguration ddb_config;
    if (ec2i.user_data_js.find("min_instance_region") != ec2i.user_data_js.end())
      ddb_config.region = ec2i.user_data_js["min_instance_region"];
    else
      ddb_config.region = ec2i.default_region;
    ddb_config.executor = ec2i.executor;
    Aws::DynamoDB::DynamoDBClient ddbc(ddb_config);

    Aws::DynamoDB::Model::AttributeValue primary_key;
    primary_key.SetS(ec2i.user_data_js["min_instance_primary_key_value"]);
    Aws::DynamoDB::Model::GetItemRequest req;
    req.WithTableName(ec2i.user_data_js["min_instance_table_name"])
        .AddKey(ec2i.user_data_js["min_instance_primary_key_name"], primary_key);
    auto outcome = ddbc.GetItem(req);
    if (outcome.IsSuccess())
    {
      auto map = outcome.GetResult().GetItem();
      auto it = map.find("min_instances");
      if (it != map.end())
      {
        auto min_instances = std::atoi(it->second.GetN().c_str());
        std::string instance_name = ec2i.user_data_js["min_instance_name"];

        Aws::Client::ClientConfiguration ec2_config;
        ec2_config.region = ec2i.default_region;
        ec2_config.executor = ec2i.executor;
        Aws::EC2::EC2Client ec2(ec2_config);

        Aws::EC2::Model::DescribeInstancesRequest request;
        Aws::EC2::Model::Filter filter1;
        filter1.SetName("tag:Name");
        filter1.AddValues(instance_name);
        request.AddFilters(filter1);
        Aws::EC2::Model::Filter filter2;
        filter2.SetName("instance-state-name");
        filter2.AddValues("running");
        request.AddFilters(filter2);

        bool done = false;
        int total_instances = 0;
        while (!done)
        {
          auto outcome = ec2.DescribeInstances(request);
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

} // namespace

void run_worker(const ec2_info &ec2i)
{
  Aws::Client::ClientConfiguration config;
  if (ec2i.user_data_js.find("task_queue_region") != ec2i.user_data_js.end())
    config.region = ec2i.user_data_js["task_queue_region"];
  else
    config.region = ec2i.default_region;
  config.executor = ec2i.executor;

  std::string queue_url = ec2i.user_data_js["task_queue_url"];

  Aws::SQS::SQSClient client(config);
  std::mutex keep_alive_mutex;
  std::map<Aws::String, Aws::String> keep_alive_set;
  bool stop = false;
  auto timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  struct itimerspec t_spec = {0};
  t_spec.it_value = cur_time() + 30;
  timerfd_settime(timerfd, TFD_TIMER_ABSTIME, &t_spec, 0);

  std::thread keep_alive_thread([&client, &keep_alive_mutex, &keep_alive_set,
                                 &stop, &timerfd, &queue_url] {
    while (true)
    {
      struct pollfd pfd;
      pfd.fd = timerfd;
      pfd.events = POLLIN;
      poll(&pfd, 1, 31000);

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
          ent.WithReceiptHandle(it.first).WithVisibilityTimeout(60).WithId(str.str());
          entries_v.back().push_back(ent);
        }
        l.unlock();
        for (auto &entries : entries_v)
        {
          req.WithQueueUrl(queue_url).WithEntries(entries);
          auto outcome = client.ChangeMessageVisibilityBatch(req);
          if (!outcome.IsSuccess())
            anon_log("ChangeMessageVisibilityBatch failed: " << outcome.GetError());
        }
      }

      struct itimerspec t_spec = {0};
      t_spec.it_value = cur_time() + 30;
      timerfd_settime(timerfd, TFD_TIMER_ABSTIME, &t_spec, 0);
    }
  });

  while (true)
  {
    Aws::SQS::Model::ReceiveMessageRequest req;
    // be careful with the timeout for now.
    // we are using the default libcurl and it seems to have a timeout
    // that is less than 10 seconds.  If we set this value to 10 seconds
    // the aws code internally sees a timeout error from libcurl, which it
    // then decides to retry the request.  To get it to return from this
    // function we need to set the timeout less than whatever the libcurl
    // timeout is set to.
    req.WithQueueUrl(queue_url).WithMaxNumberOfMessages(1).WithWaitTimeSeconds(5);
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
      timerfd_settime(timerfd, TFD_TIMER_ABSTIME, &t_spec, 0);

      for (auto &m : messages)
      {
        auto bash_cmd = get_body(m);

        const auto max_retries = 2;

        const auto &att = m.GetAttributes();
        auto arc = att.find(Aws::SQS::Model::MessageSystemAttributeName::ApproximateReceiveCount);
        auto approx_receive_count = 1000;
        if (arc != att.end())
          approx_receive_count = std::stoull(arc->second);

        auto pid = fork();
        if (pid == -1)
          do_error("fork()");
        if (pid != 0)
        {
          // the calling parent
          int status;
          auto w = waitpid(-1, &status, WUNTRACED | WCONTINUED);
          if (w == -1)
            do_error("waitpid(-1, &status, WUNTRACED | WCONTINUED)");

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
            req.WithQueueUrl(queue_url).WithReceiptHandle(m.GetReceiptHandle());
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
      // no messages after 10 seconds.  Check for shutdown
      anon_log("no messages after wating period");
      if (should_shut_down(ec2i))
      {
        anon_log("no reason to keep running, shutting down");
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
