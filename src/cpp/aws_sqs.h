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

#pragma once

#include <aws/core/Aws.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/sqs/SQSClient.h>
#include <aws/sqs/model/ReceiveMessageRequest.h>
#include <aws/sqs/model/ReceiveMessageResult.h>
#include <aws/sqs/model/DeleteMessageRequest.h>
#include <aws/sqs/model/ChangeMessageVisibilityRequest.h>
#include <aws/sqs/model/ChangeMessageVisibilityBatchRequest.h>
#include <aws/sqs/model/ChangeMessageVisibilityBatchRequestEntry.h>
#include <iostream>
#include <atomic>
#include <string>
#include "fiber.h"
#include "nlohmann/json.hpp"

class aws_sqs_listener : std::enable_shared_from_this<aws_sqs_listener>
{
public:
  aws_sqs_listener(const std::shared_ptr<Aws::Auth::AWSCredentialsProvider> &provider,
                   const Aws::Client::ClientConfiguration &client_config,
                   const Aws::String &queue_url,
                   const std::function<bool(const Aws::SQS::Model::Message &m)> &handler);
  ~aws_sqs_listener();

  void start();
  static std::function<bool(const Aws::SQS::Model::Message &m)> js_wrap(const std::function<void(const nlohmann::json &body)> &fn);

private:
  Aws::SQS::SQSClient _client;
  Aws::String _queue_url;
  fiber_mutex _mtx;
  fiber_cond _num_fibers_cond;
  std::atomic<int> _num_fibers;
  std::atomic<bool> _exit_now;
  std::function<bool(const Aws::SQS::Model::Message &m)> _process_msg;
  std::map<Aws::String, Aws::String> _alive_set;
  io_dispatch::scheduled_task _timer_task;

  enum
  {
    visibility_sweep_time = 30,
    visibility_time = 60,
    visibility_immediate_retry_time = 2,
    max_in_flight_fibers = 1000,
    read_wait_time = 10,
    max_messages_per_read = 10
  };

  void set_visibility_timeout();
  void add_to_keep_alive(const Aws::SQS::Model::Message &);
  void remove_from_keep_alive(const Aws::SQS::Model::Message &, bool reset_visibility);
  void delete_message(const Aws::SQS::Model::Message &);
  void start_listen();
};
