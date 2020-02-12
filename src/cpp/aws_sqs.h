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

class aws_sqs_listener : public std::enable_shared_from_this<aws_sqs_listener>
{
  static Aws::String replace_all(Aws::String& s, const Aws::String& pat, const Aws::String& rep)
  {
    size_t pos = 0;
    auto plen = pat.size();
    auto rlen = rep.size();
    while (true) {
      auto fpos = s.find(pat, pos);
      if (fpos == Aws::String::npos)
        break;
      s = s.replace(fpos, plen, rep);
      pos = fpos + rlen;
    }
    return s;
  }

public:
  template <typename Fn>
  static std::shared_ptr<aws_sqs_listener> new_listener(const std::shared_ptr<Aws::Auth::AWSCredentialsProvider> &provider,
                                                        const Aws::Client::ClientConfiguration &client_config,
                                                        const Aws::String &queue_url,
                                                        const Fn &handler,
                                                        int max_read_messages = max_messages_per_read,
                                                        bool single_concurrent_message = false,
                                                        size_t stack_size = _default_process_message_stack_size)
  {
    auto ths = std::make_shared<aws_sqs_listener>(provider, client_config, queue_url, wrap(handler), max_read_messages, single_concurrent_message, stack_size);
    ths->start();
    return ths;
  }

  ~aws_sqs_listener();

  static std::function<bool(const Aws::SQS::Model::Message &m)> js_wrap(const std::function<bool(const Aws::SQS::Model::Message &m, const nlohmann::json &body)> &fn);
  static std::function<bool(const Aws::SQS::Model::Message &m, const std::function<void(bool delete_it, int visibility_timeout)>& del)> js_wrap(const std::function<bool(const Aws::SQS::Model::Message &m, const std::function<void(bool delete_it, int visibility_timeout)>& del, const nlohmann::json &body)> &fn);

  class inval_message : public std::runtime_error
  {
  public:
    inval_message(const std::string& message)
      : std::runtime_error(message)
    {}
  };

  void on_timeout(const std::function<bool()>& timeout) {
    _continue_after_timeout = timeout;
  }

  // I wish I could find documentation on this, but at least the c++ SQS amazon
  // code returns the Message.GetBody with XML-style encoding of the characters
  // &, ', ", >, and <
  // No one ever wants that...
  static std::string get_body(const Aws::SQS::Model::Message &m)
  {
    auto body = m.GetBody();
    body = replace_all(body, "&amp;", "&");
    body = replace_all(body, "&quot;", "\"");
    body = replace_all(body, "&apos;", "\'");
    body = replace_all(body, "&lt;", "<");
    return replace_all(body, "&gt;", ">").c_str();
  }

private:
  enum
  {
    _default_process_message_stack_size = 48 * 1024 - 256,
    _simple_stack_size = 16 * 1024 - 256
  };

  void start();

  Aws::SQS::SQSClient _client;
  Aws::String _queue_url;
  fiber_mutex _mtx;
  fiber_cond _num_fibers_cond;
  int _num_fibers;
  std::atomic<bool> _exit_now;
  std::function<bool(const Aws::SQS::Model::Message &m)> _process_msg;
  std::function<bool(const Aws::SQS::Model::Message &m, const std::function<void(bool delete_it, int visibility_timeout)>& delete_message)> _process_msg_del;
  std::function<bool()> _continue_after_timeout;
  std::map<Aws::String, Aws::String> _alive_set;
  io_dispatch::scheduled_task _timer_task;
  int _consecutive_errors;
  bool _single_concurrent_message;
  size_t _stack_size;
  int _max_read_messages;

  enum
  {
    visibility_sweep_time = 30,
    visibility_time = 60,
    visibility_immediate_retry_time = 5,
    max_in_flight_fibers = 1000,
    read_wait_time = 10,
    max_messages_per_read = 10
  };

  void set_visibility_timeout();
  void add_to_keep_alive(const Aws::SQS::Model::Message &);
  void remove_from_keep_alive(const Aws::SQS::Model::Message &, bool reset_visibility, int visibility_timeout);
  void delete_message(const Aws::SQS::Model::Message &);
  void start_listen();

  static std::function<bool(const Aws::SQS::Model::Message &m)> wrap(const std::function<bool(const Aws::SQS::Model::Message &m)> &fn)
  {
    return fn;
  }

  static std::function<bool(const Aws::SQS::Model::Message &m)> wrap(const std::function<bool(const Aws::SQS::Model::Message &m, const nlohmann::json &body)> &fn)
  {
    return aws_sqs_listener::js_wrap(fn);
  }

  static std::function<bool(const Aws::SQS::Model::Message &m, const std::function<void(bool delete_it, int visibility_timeout)>& del)> wrap(const std::function<bool(const Aws::SQS::Model::Message &m, const std::function<void(bool delete_it, int visibility_timeout)>& del)> &fn) 
  {
    return fn;
  }

  static std::function<bool(const Aws::SQS::Model::Message &m, const std::function<void(bool delete_it, int visibility_timeout)>& del)> wrap(const std::function<bool(const Aws::SQS::Model::Message &m, const std::function<void(bool delete_it, int visibility_timeout)>& del, const nlohmann::json &body)> &fn)
  {
    return aws_sqs_listener::js_wrap(fn);
  }

public:
  aws_sqs_listener(const std::shared_ptr<Aws::Auth::AWSCredentialsProvider> &provider,
                   const Aws::Client::ClientConfiguration &client_config,
                   const Aws::String &queue_url,
                   const std::function<bool(const Aws::SQS::Model::Message &m)> &handler,
                   int max_read_messages,
                   bool single_concurrent_message,
                   size_t stack_size);

  aws_sqs_listener(const std::shared_ptr<Aws::Auth::AWSCredentialsProvider> &provider,
                   const Aws::Client::ClientConfiguration &client_config,
                   const Aws::String &queue_url,
                   const std::function<bool(const Aws::SQS::Model::Message &m, const std::function<void(bool delete_it, int visibility_timeout)>&)> &handler,
                   int max_read_messages,
                   bool single_concurrent_message,
                   size_t stack_size);
};

class aws_sqs_sender : public std::enable_shared_from_this<aws_sqs_sender>
{
  Aws::SQS::SQSClient _client;
  Aws::String _queue_url;

public:
  aws_sqs_sender(const std::shared_ptr<Aws::Auth::AWSCredentialsProvider> &provider,
                 const Aws::Client::ClientConfiguration &client_config,
                 const Aws::String &queue_url);

  void send(const nlohmann::json &body,
            const std::function<void(const bool success, const std::string &id, const std::string &errorReason)> &response);
};
