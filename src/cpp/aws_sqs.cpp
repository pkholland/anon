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

#include "aws_sqs.h"
#include <aws/sqs/model/QueueAttributeName.h>
#include <aws/sqs/model/SendMessageRequest.h>

using namespace Aws::SQS;
using json = nlohmann::json;

aws_sqs_listener::aws_sqs_listener(const std::shared_ptr<Aws::Auth::AWSCredentialsProvider> &provider,
                                   const Aws::Client::ClientConfiguration &client_config,
                                   const Aws::String &queue_url,
                                   const std::function<bool(const Aws::SQS::Model::Message &m)> &handler,
                                   bool single_concurrent_message,
                                   size_t stack_size)
    : _client(provider, client_config),
      _queue_url(queue_url),
      _num_fibers(0),
      _exit_now(false),
      _process_msg(handler),
      _consecutive_errors(0),
      _single_concurrent_message(single_concurrent_message),
      _stack_size(stack_size),
      _continue_after_timeout([]{return true;})
{
}

aws_sqs_listener::aws_sqs_listener(const std::shared_ptr<Aws::Auth::AWSCredentialsProvider> &provider,
                                   const Aws::Client::ClientConfiguration &client_config,
                                   const Aws::String &queue_url,
                                   const std::function<bool(const Aws::SQS::Model::Message &m, const std::function<void()>&)> &handler,
                                   bool single_concurrent_message,
                                   size_t stack_size)
    : _client(provider, client_config),
      _queue_url(queue_url),
      _num_fibers(0),
      _exit_now(false),
      _process_msg_del(handler),
      _consecutive_errors(0),
      _single_concurrent_message(single_concurrent_message),
      _stack_size(stack_size),
      _continue_after_timeout([]{return true;})
{
}

void aws_sqs_listener::start()
{
  std::weak_ptr<aws_sqs_listener> wp = shared_from_this();

  fiber::run_in_fiber(
      [wp] {
        auto ths = wp.lock();
        if (ths)
          ths->start_listen();
      },
      aws_sqs_listener::_simple_stack_size, "aws_sqs_listener::aws_sqs_listener, start_listen");

  #if defined(ANON_DEBUG_TIMERS)
  anon_log("io_dispatch::schedule_task, aws_sqs_listener::start");
  #endif
  _timer_task = io_dispatch::schedule_task(
      [wp] {
        fiber::run_in_fiber(
            [wp] {
              auto ths = wp.lock();
              if (ths)
                ths->set_visibility_timeout();
            },
            aws_sqs_listener::_simple_stack_size, "aws_sqs_listener, set_visibility_timeout sweeper");
      },
      cur_time() + visibility_sweep_time);
}

aws_sqs_listener::~aws_sqs_listener()
{
  {
    _exit_now = true;
    fiber_lock l(_mtx);
    while (_num_fibers > 0)
      _num_fibers_cond.wait(l);
  }
  io_dispatch::remove_task(_timer_task);
}

std::function<bool(const Aws::SQS::Model::Message &m)> aws_sqs_listener::js_wrap(const std::function<bool(const Aws::SQS::Model::Message &m, const nlohmann::json &body)> &fn)
{
  return [fn](const Aws::SQS::Model::Message &m) -> bool {
    std::string body = get_body(m);
    try
    {
      json body_js = json::parse(body.begin(), body.end());
      try
      {
        return fn(m, body_js);
      }
      catch (const inval_message& exc)
      {
        anon_log_error("caught exception processing message: " << exc.what() << ", message body: '" << body << "'");
        return true;
      }
      catch (const std::exception &exc)
      {
        anon_log_error("caught exception processing message: " << exc.what() << ", message body: '" << body << "'");
        return false;
      }
      catch (...)
      {
        anon_log_error("caught unknown exception processing message, message body: '" << body << "'");
        return false;
      }
    }
    catch (const std::exception &exc)
    {
      anon_log_error("caught exception parsing message: " << exc.what() << ", message body: '" << body << "'");
      return true;
    }
    catch (...)
    {
      anon_log_error("caught unknown exception parsing message, message body: '" << body << "'");
      return true;
    }
  };
}

std::function<bool(const Aws::SQS::Model::Message &m, const std::function<void()>& del)> aws_sqs_listener::js_wrap(const std::function<bool(const Aws::SQS::Model::Message &m, const std::function<void()>& del, const nlohmann::json &body)> &fn)
{
  return [fn](const Aws::SQS::Model::Message &m, const std::function<void()>& del) -> bool {
    std::string body = get_body(m);
    try
    {
      json body_js = json::parse(body.begin(), body.end());
      try
      {
        return fn(m, del, body_js);
      }
      catch (const inval_message& exc)
      {
        anon_log_error("caught exception processing message: " << exc.what() << ", message body: '" << body << "'");
        del();
        return true;
      }
      catch (const std::exception &exc)
      {
        anon_log_error("caught exception processing message: " << exc.what() << ", message body: '" << body << "'");
        return false;
      }
      catch (...)
      {
        anon_log_error("caught unknown exception processing message, message body: '" << body << "'");
        return false;
      }
    }
    catch (const std::exception &exc)
    {
      anon_log_error("caught exception parsing message: " << exc.what() << ", message body: '" << body << "'");
      del();
      return true;
    }
    catch (...)
    {
      anon_log_error("caught unknown exception parsing message, message body: '" << body << "'");
      del();
      return true;
    }
  };
}

void aws_sqs_listener::start_listen()
{
  Model::ReceiveMessageRequest req;
  req.WithQueueUrl(_queue_url).WithMaxNumberOfMessages(_single_concurrent_message ? 1 : max_messages_per_read).WithWaitTimeSeconds(read_wait_time);
  Aws::Vector<Model::QueueAttributeName> att;
  att.push_back(Model::QueueAttributeName::All);
  req.WithAttributeNames(std::move(att));
  std::weak_ptr<aws_sqs_listener> wp = shared_from_this();
  _client.ReceiveMessageAsync(req, [wp](const SQSClient *, const Model::ReceiveMessageRequest &, const Model::ReceiveMessageOutcome &out, const std::shared_ptr<const Aws::Client::AsyncCallerContext> &) {
    auto ths = wp.lock();
    if (!ths)
      return;
    fiber::rename_fiber("aws_sqs_listener::start_listen, ReceiveMessageAsync");
    if (!out.IsSuccess())
    {
      ++ths->_consecutive_errors;
      if (ths->_consecutive_errors > 10)
      {
        anon_log_error("SQS ReceiveMessage failed, _consecutive_errors: " << ths->_consecutive_errors << "\n"
                                                                          << out.GetError());
      }
      else
      {
        anon_log("SQS ReceiveMessage failed, _consecutive_errors: " << ths->_consecutive_errors);
      }
      
      fiber::msleep(2000);
    }
    else
    {
      ths->_consecutive_errors = 0;
      auto &messages = out.GetResult().GetMessages();
      auto num_messages = messages.size();

      if (num_messages > 0)
      {
        ths->_num_fibers += num_messages;
        for (auto &m : messages)
          fiber::run_in_fiber(
              [wp, m] {
                auto ths = wp.lock();
                if (!ths)
                  return;
                ths->add_to_keep_alive(m);
                bool success;
                if (ths->_process_msg) {
                  success = ths->_process_msg(m);
                  if (success)
                    ths->delete_message(m);
                }
                else
                  success = ths->_process_msg_del(m, [wp, m] {
                    auto ths = wp.lock();
                    if (ths)
                      ths->delete_message(m);
                  });
                if (!success)
                  ths->remove_from_keep_alive(m, true);
                if (ths->_single_concurrent_message && !ths->_exit_now)
                  ths->start_listen();
              },
              ths->_stack_size, "aws_sqs_listener, process_msg");
      } else {
        if (!ths->_continue_after_timeout())
          ths->_exit_now = true;
      }
    }

    if (ths->_consecutive_errors < 1000)
    {
      if (!ths->_single_concurrent_message && !ths->_exit_now)
      {
        fiber_lock l(ths->_mtx);
        while (ths->_num_fibers >= max_in_flight_fibers)
          ths->_num_fibers_cond.wait(l);
        fiber::run_in_fiber(
            [wp] {
              auto ths = wp.lock();
              if (ths)
                ths->start_listen();
            },
            aws_sqs_listener::_simple_stack_size, "aws_sqs_listener, restart_listen");
      }
    }
    else
      anon_log_error("too many consecutive errors, giving up...");
  });
}

void aws_sqs_listener::set_visibility_timeout()
{
  fiber_lock l(_mtx);
  auto numMessages = _alive_set.size();
  if (numMessages > 0)
  {
    Model::ChangeMessageVisibilityBatchRequest req;
    Aws::Vector<Aws::Vector<Model::ChangeMessageVisibilityBatchRequestEntry>> entries_v;
    int index = 0;
    for (auto it : _alive_set)
    {
      Model::ChangeMessageVisibilityBatchRequestEntry ent;
      std::ostringstream str;
      if (index % 10 == 0)
        entries_v.push_back(Aws::Vector<Model::ChangeMessageVisibilityBatchRequestEntry>());
      str << "message_" << ++index;
      ent.WithReceiptHandle(it.first).WithVisibilityTimeout(visibility_time).WithId(str.str());
      entries_v.back().push_back(ent);
    }
    for (auto &entries : entries_v)
    {
      req.WithQueueUrl(_queue_url).WithEntries(entries);
      auto nMessages = entries.size();
      _client.ChangeMessageVisibilityBatchAsync(req, [nMessages](const SQSClient *, const Model::ChangeMessageVisibilityBatchRequest &, const Model::ChangeMessageVisibilityBatchOutcome &out, const std::shared_ptr<const Aws::Client::AsyncCallerContext> &) {
        fiber::rename_fiber("aws_sqs_listener::set_visibility_timeout, ChangeMessageVisibilityBatchAsync");
        if (out.IsSuccess())
        {
          anon_log("batch visibilty reset for " << nMessages << " messages");
        }
        else
        {
          do_error("batch reset visibility for " << nMessages << " messages: " << out.GetError().GetMessage());
        }
      });
    }
  }

  // schedule the sweep to run again in visibility_sweep_time seconds
  std::weak_ptr<aws_sqs_listener> wp = shared_from_this();
  #if defined(ANON_DEBUG_TIMERS)
  anon_log("io_dispatch::schedule_task, aws_sqs_listener::set_visibility_timeout");
  #endif
  _timer_task = io_dispatch::schedule_task(
      [wp] {
        fiber::run_in_fiber(
            [wp] {
              auto ths = wp.lock();
              if (ths)
                ths->set_visibility_timeout();
            },
            aws_sqs_listener::_simple_stack_size, "aws_sqs_listener, set_visibility_timeout sweeper");
      },
      cur_time() + visibility_sweep_time);
}

void aws_sqs_listener::add_to_keep_alive(const Model::Message &m)
{
  fiber_lock l(_mtx);
  _alive_set[m.GetReceiptHandle()] = m.GetMessageId();
}

void aws_sqs_listener::remove_from_keep_alive(const Model::Message &m, bool reset_visibility)
{
  {
    fiber_lock l(_mtx);
    _alive_set.erase(m.GetReceiptHandle());
  }
  if (reset_visibility)
  {
    // encourage the system to try this message again soon by resetting its visility near 0
    Model::ChangeMessageVisibilityRequest req;
    req.WithQueueUrl(_queue_url).WithReceiptHandle(m.GetReceiptHandle()).WithVisibilityTimeout(visibility_immediate_retry_time);
    auto messageId = m.GetMessageId();
    _client.ChangeMessageVisibilityAsync(req, [messageId](const SQSClient *, const Model::ChangeMessageVisibilityRequest &, const Model::ChangeMessageVisibilityOutcome &out, const std::shared_ptr<const Aws::Client::AsyncCallerContext> &) {
      fiber::rename_fiber("aws_sqs_listener::remove_from_keep_alive, ChangeMessageVisibilityAsync");
      if (out.IsSuccess())
      {
        // anon_log("reset message visibility near 0 for " << messageId);
      }
      else
      {
        do_error("reset message visibility near 0 for " << messageId << ", " << out.GetError().GetMessage());
      }
    });
  }
}

void aws_sqs_listener::delete_message(const Model::Message &m)
{
  remove_from_keep_alive(m, false);
  Model::DeleteMessageRequest req;
  req.WithQueueUrl(_queue_url).WithReceiptHandle(m.GetReceiptHandle());
  auto messageId = m.GetMessageId();
  _client.DeleteMessageAsync(req, [messageId](const SQSClient *, const Model::DeleteMessageRequest &, const Model::DeleteMessageOutcome &out, const std::shared_ptr<const Aws::Client::AsyncCallerContext> &) {
    fiber::rename_fiber("aws_sqs_listener::delete_message, DeleteMessageAsync");
    if (out.IsSuccess())
    {
      // anon_log("deleted SQS message " << messageId);
    }
    else
    {
      anon_log("delete SQS message failed, messageId:" << messageId << ", " << out.GetError().GetMessage());
    }
  });
}

aws_sqs_sender::aws_sqs_sender(const std::shared_ptr<Aws::Auth::AWSCredentialsProvider> &provider,
                               const Aws::Client::ClientConfiguration &client_config,
                               const Aws::String &queue_url)
    : _client(provider, client_config),
      _queue_url(queue_url)
{
}

void aws_sqs_sender::send(const json &body,
                          const std::function<void(const bool success, const std::string &id, const std::string &errorReason)> &response)
{
  Model::SendMessageRequest req;
  req.WithQueueUrl(_queue_url).WithMessageBody(Aws::String(body.dump()));
  _client.SendMessageAsync(req, [response](const SQSClient *, const Model::SendMessageRequest &, const Model::SendMessageOutcome &outcome, const std::shared_ptr<const Aws::Client::AsyncCallerContext> &) {
    fiber::rename_fiber("aws_sqs_sender::send, SendMessageAsync");
    response(outcome.IsSuccess(),
             std::string(outcome.GetResult().GetMessageId()),
             std::string(outcome.GetError().GetMessage()));
  });
}
