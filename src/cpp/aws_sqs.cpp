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
#include "aws_throttle.h"
#include "aws_client.h"
#include <aws/sqs/model/QueueAttributeName.h>
#include <aws/sqs/model/SendMessageRequest.h>

using namespace Aws::SQS;
using json = nlohmann::json;

aws_sqs_listener::aws_sqs_listener(const std::shared_ptr<Aws::Auth::AWSCredentialsProvider> &provider,
                                   const Aws::Client::ClientConfiguration &client_config,
                                   const Aws::String &queue_url,
                                   const std::function<bool(const Aws::SQS::Model::Message &m)> &handler,
                                   int max_read_messages,
                                   bool single_concurrent_message,
                                   size_t stack_size)
    : _client(provider, client_config),
      _queue_url(queue_url),
      _num_fibers(0),
      _exit_now(false),
      _process_msg(handler),
      _consecutive_errors(0),
      _max_read_messages(max_read_messages),
      _single_concurrent_message(single_concurrent_message),
      _stack_size(stack_size),
      _continue_after_timeout([] { return true; })
{
}

aws_sqs_listener::aws_sqs_listener(const std::shared_ptr<Aws::Auth::AWSCredentialsProvider> &provider,
                                   const Aws::Client::ClientConfiguration &client_config,
                                   const Aws::String &queue_url,
                                   const std::function<bool(const Aws::SQS::Model::Message &m, const std::function<void(bool delete_it, int visibility_timeout)> &)> &handler,
                                   int max_read_messages,
                                   bool single_concurrent_message,
                                   size_t stack_size)
    : _client(provider, client_config),
      _queue_url(queue_url),
      _num_fibers(0),
      _exit_now(false),
      _process_msg_del(handler),
      _consecutive_errors(0),
      _max_read_messages(max_read_messages),
      _single_concurrent_message(single_concurrent_message),
      _stack_size(stack_size),
      _continue_after_timeout([] { return true; })
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
  anon_log("fiber::schedule_task, aws_sqs_listener::start");
#endif
  _timer_task = fiber::schedule_task([wp] {
    auto ths = wp.lock();
    if (ths)
      ths->set_visibility_timeout();
  },
                                     cur_time() + visibility_sweep_time, aws_sqs_listener::_simple_stack_size, "aws_sqs_listener, set_visibility_timeout sweeper");
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
      catch (const inval_message &exc)
      {
        anon_log_error("caught exception processing message: " << exc.what() << ", message body: '" << body << "'");
        return true;
      }
      catch (const std::exception &exc)
      {
        anon_log_error("caught exception processing message: " << exc.what() << ", message body: '" << body << "'");
        return false;
      }
    }
    catch (const std::exception &exc)
    {
      anon_log_error("caught exception parsing message: " << exc.what() << ", message body: '" << body << "'");
      return true;
    }
  };
}

std::function<bool(const Aws::SQS::Model::Message &m, const std::function<void(bool delete_it, int visibility_timeout)> &del)> aws_sqs_listener::js_wrap(const std::function<bool(const Aws::SQS::Model::Message &m, const std::function<void(bool delete_it, int visibility_timeout)> &del, const nlohmann::json &body)> &fn)
{
  return [fn](const Aws::SQS::Model::Message &m, const std::function<void(bool delete_it, int visibility_timeout)> &del) -> bool {
    std::string body = get_body(m);
    try
    {
      json body_js = json::parse(body.begin(), body.end());
      try
      {
        return fn(m, del, body_js);
      }
      catch (const inval_message &exc)
      {
        anon_log_error("caught exception processing message: " << exc.what() << ", message body: '" << body << "'");
        del(true, 0);
        return true;
      }
      catch (const std::exception &exc)
      {
        anon_log_error("caught exception processing message: " << exc.what() << ", message body: '" << body << "'");
        return false;
      }
    }
    catch (const std::exception &exc)
    {
      anon_log_error("caught exception parsing message: " << exc.what() << ", message body: '" << body << "'");
      del(true, 0);
      return true;
    }
  };
}

void aws_sqs_listener::start_listen()
{
  Model::ReceiveMessageRequest req;
  req.WithQueueUrl(_queue_url).WithMaxNumberOfMessages(_single_concurrent_message ? 1 : _max_read_messages).WithWaitTimeSeconds(read_wait_time);
  Aws::Vector<Model::QueueAttributeName> att;
  att.push_back(Model::QueueAttributeName::All);
  req.WithAttributeNames(std::move(att));
  std::weak_ptr<aws_sqs_listener> wp = shared_from_this();
#if EXTENSIVE_AWS_LOGS > 0
  anon_log("aws_sqs, calling ReceiveMessageAsync");
#endif
  _client.ReceiveMessageAsync(req, [wp](const SQSClient *, const Model::ReceiveMessageRequest &, const Model::ReceiveMessageOutcome &out, const std::shared_ptr<const Aws::Client::AsyncCallerContext> &) {
    auto ths = wp.lock();
    if (!ths)
      return;
    bool last_read_failed = false;
    fiber::rename_fiber("aws_sqs_listener::start_listen, ReceiveMessageAsync");
    if (!out.IsSuccess())
    {
      ++ths->_consecutive_errors;
      if (ths->_consecutive_errors > 10)
      {
        anon_log_error("aws_sqs, SQS ReceiveMessage failed, _consecutive_errors: " << ths->_consecutive_errors << ", "
                                                                                   << out.GetError().GetMessage());
      }
      else
      {
        anon_log("aws_sqs, SQS ReceiveMessage failed, _consecutive_errors: " << ths->_consecutive_errors);
      }

      fiber::msleep(2000);
      last_read_failed = true;
    }
    else
    {
      ths->_consecutive_errors = 0;
      auto &messages = out.GetResult().GetMessages();
      auto num_messages = messages.size();

#if EXTENSIVE_AWS_LOGS > 0
      anon_log("aws_sqs, ReceiveMessageAsync completed sucessfully with " << num_messages << " messages");
#endif

      if (num_messages > 0)
      {
        fiber_lock l(ths->_mtx);
        ths->_num_fibers += num_messages;
        for (auto &m : messages)
          fiber::run_in_fiber(
              [wp, m] {
                auto ths = wp.lock();
                if (!ths)
                  return;
                try
                {
                  ths->add_to_keep_alive(m);
                  bool success;
                  if (ths->_process_msg)
                  {
                    success = ths->_process_msg(m);
                    if (success)
                      ths->delete_message(m);
#if EXTENSIVE_AWS_LOGS > 0
                    else
                      anon_log("aws_sqs, _process_msg returned false");
#endif
                    if (ths->_single_concurrent_message && !ths->_exit_now)
                      ths->start_listen();
                  }
                  else
                    success = ths->_process_msg_del(m, [wp, m](bool delete_it, int visibility_timeout) {
#if EXTENSIVE_AWS_LOGS > 0
                      anon_log("aws_sqs, _process_msg_del callback made with delete_it: " << (delete_it ? "true" : "false") << ", visibility_timeout: " << visibility_timeout);
#endif
                      auto ths = wp.lock();
                      if (ths)
                      {
                        if (delete_it)
                          ths->delete_message(m);
                        else
                          ths->remove_from_keep_alive(m, true, visibility_timeout);
                        if (ths->_single_concurrent_message && !ths->_exit_now)
                          ths->start_listen();
                      }
                    });
                  if (!success)
                    ths->remove_from_keep_alive(m, true, visibility_immediate_retry_time);
                }
                catch (const aws_throttle_error &exc)
                {
                  Model::SendMessageRequest req;
                  req.WithQueueUrl(ths->_queue_url)
                    .WithMessageBody(m.GetBody())
                    .WithDelaySeconds(5);
                  aws_throttle(aws_get_default_region(), [&]{
                    auto outcome = ths->_client.SendMessage(req);
                    aws_check(outcome, "sqs.SendMessage");
                  });
                  ths->delete_message(m);
                }
                catch (const std::exception &exc)
                {
                  anon_log("aws_sqs, exception thrown during message dispatch, error: " << exc.what());
                }
                catch (...)
                {
                  anon_log("aws_sqs, unknown exception thrown during message dispatch");
                }
                fiber_lock l(ths->_mtx);
                --ths->_num_fibers;
#if EXTENSIVE_AWS_LOGS > 0
                anon_log("aws_sqs, message dispatch complete, num outstanding messages: " << ths->_num_fibers);
#endif
                ths->_num_fibers_cond.notify_all();
              },
              ths->_stack_size, "aws_sqs_listener, process_msg");
      }
      else
      {
        if (!ths->_continue_after_timeout())
          ths->_exit_now = true;
      }
    }

    if (ths->_consecutive_errors < 1000)
    {
      if ((last_read_failed || !ths->_single_concurrent_message || out.GetResult().GetMessages().size() == 0) && !ths->_exit_now)
      {
        fiber_lock l(ths->_mtx);
        while (ths->_num_fibers >= max_in_flight_fibers)
        {
#if EXTENSIVE_AWS_LOGS > 0
          anon_log("aws_sqs, stalling call to restart_listen because _num_fibers == " << ths->_num_fibers << ", (max_in_flight_fibers == " << max_in_flight_fibers << ")");
#endif
          ths->_num_fibers_cond.wait(l);
        }
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
      ent.WithReceiptHandle(it.first).WithVisibilityTimeout(visibility_time).WithId(str.str().c_str());
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
#if EXTENSIVE_AWS_LOGS > 0
          anon_log("aws_sqs, batch visibilty reset for " << nMessages << " message" << (nMessages > 1 ? "s" : ""));
#endif
        }
        else
        {
          do_error("aws_sqs, batch reset visibility for " << nMessages << " messages: " << out.GetError().GetMessage());
        }
      });
    }
  }

  // schedule the sweep to run again in visibility_sweep_time seconds
  std::weak_ptr<aws_sqs_listener> wp = shared_from_this();
#if defined(ANON_DEBUG_TIMERS)
  anon_log("fiber::schedule_task, aws_sqs_listener::set_visibility_timeout");
#endif
  _timer_task = fiber::schedule_task([wp] {
    auto ths = wp.lock();
    if (ths)
      ths->set_visibility_timeout();
  },
                                     cur_time() + visibility_sweep_time, aws_sqs_listener::_simple_stack_size, "aws_sqs_listener, set_visibility_timeout sweeper");
}

void aws_sqs_listener::add_to_keep_alive(const Model::Message &m)
{
  fiber_lock l(_mtx);
  _alive_set[m.GetReceiptHandle()] = m.GetMessageId();
}

void aws_sqs_listener::remove_from_keep_alive(const Model::Message &m, bool reset_visibility, int visibility_timeout)
{
  {
    fiber_lock l(_mtx);
    _alive_set.erase(m.GetReceiptHandle());
  }

  if (reset_visibility)
  {
    Model::ChangeMessageVisibilityRequest req;
    req.WithQueueUrl(_queue_url).WithReceiptHandle(m.GetReceiptHandle()).WithVisibilityTimeout(visibility_timeout);
    auto messageId = m.GetMessageId();
    _client.ChangeMessageVisibilityAsync(req, [messageId](const SQSClient *, const Model::ChangeMessageVisibilityRequest &r, const Model::ChangeMessageVisibilityOutcome &out, const std::shared_ptr<const Aws::Client::AsyncCallerContext> &) {
      fiber::rename_fiber("aws_sqs_listener::remove_from_keep_alive, ChangeMessageVisibilityAsync");
      if (out.IsSuccess())
      {
        // anon_log("reset message visibility to " << r.GetVisibilityTimeout() << " for " << messageId);
      }
      else
      {
        do_error("reset message visibility to " << r.GetVisibilityTimeout() << " for " << messageId << ", " << out.GetError().GetMessage());
      }
    });
  }
}

void aws_sqs_listener::delete_message(const Model::Message &m)
{
  Model::DeleteMessageRequest req;
  req.WithQueueUrl(_queue_url).WithReceiptHandle(m.GetReceiptHandle());
  auto messageId = m.GetMessageId();
  std::weak_ptr<aws_sqs_listener> wp = shared_from_this();
  _client.DeleteMessageAsync(req, [messageId, wp, m](const SQSClient *, const Model::DeleteMessageRequest &, const Model::DeleteMessageOutcome &out, const std::shared_ptr<const Aws::Client::AsyncCallerContext> &) {
    fiber::rename_fiber("aws_sqs_listener::delete_message, DeleteMessageAsync");
    if (out.IsSuccess())
    {
#if EXTENSIVE_AWS_LOGS > 0
      anon_log("aws_sqs, deleted SQS message " << messageId);
#endif
    }
    else
    {
      anon_log("aws_sqs, delete SQS message failed, messageId:" << messageId << ", " << out.GetError().GetMessage());
    }
    auto ths = wp.lock();
    if (ths)
      ths->remove_from_keep_alive(m, false, 0);
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
  req.WithQueueUrl(_queue_url).WithMessageBody(body.dump().c_str());
  _client.SendMessageAsync(req, [response](const SQSClient *, const Model::SendMessageRequest &, const Model::SendMessageOutcome &outcome, const std::shared_ptr<const Aws::Client::AsyncCallerContext> &) {
    fiber::rename_fiber("aws_sqs_sender::send, SendMessageAsync");
    response(outcome.IsSuccess(),
             outcome.GetResult().GetMessageId().c_str(),
             outcome.GetError().GetMessage().c_str());
  });
}
