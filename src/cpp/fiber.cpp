/*
 Copyright (c) 2015 Anon authors, see AUTHORS file.
 
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

#include "fiber.h"
#include "time_utils.h"
#include <fcntl.h>

#if defined(ANON_LOG_KEEP_RECENT)
recent_logs recent_logs::singleton;
#endif

thread_local io_params tls_io_params;

// suspend the calling fiber on the fiber_cond's wake list
// then jump back to the parent fiber, telling it to unlock
// the mutex.  When we return (after some other fiber calls
// one of the notify functions), attempt to lock the mutex.
void fiber_cond::wait(fiber_lock &lock)
{
  anon::assert_no_locks();

  auto params = &tls_io_params;
  auto f = params->current_fiber_;

  f->next_wake_ = 0;
  if (wake_head_)
  {
    auto last = wake_head_;
    while (last->next_wake_)
      last = last->next_wake_;
    last->next_wake_ = f;
  }
  else
    wake_head_ = f;

  params->opcode_ = io_params::oc_cond_wait;
  params->cond_mutex_ = &lock.mutex_;
  f->switch_to_fiber(params->parent_fiber_);
  lock.mutex_.lock();
}

// move all of this fiber_cond's suspended fibers
// to the current thread's wake list
void fiber_cond::notify_all()
{
  if (wake_head_)
  {
    auto last = wake_head_;
    while (last->next_wake_)
      last = last->next_wake_;
    auto params = &tls_io_params;
    last->next_wake_ = params->wake_head_;
    params->wake_head_ = wake_head_;
    wake_head_ = 0;
  }
}

// move one of this fiber_cond's suspended fibers
// to the current thread's wake list
void fiber_cond::notify_one()
{
  if (wake_head_)
  {
    auto params = &tls_io_params;
    auto next = wake_head_->next_wake_;
    wake_head_->next_wake_ = params->wake_head_;
    params->wake_head_ = wake_head_;
    wake_head_ = next;
  }
}

/////////////////////////////////////////////////////

void fiber_mutex::sleep_till_woken()
{
  auto params = &tls_io_params;

  // Add this fiber to this mutex's wakeList and then suspend the fiber.
  // Note that the decrement of state_ happens after we have switched to
  // the iod fiber.
  fiber *curF = params->current_fiber_;
  if (!fiber_wake_tail_)
    fiber_wake_tail_ = curF;
  curF->next_wake_ = fiber_wake_head_;
  fiber_wake_head_ = curF;

  // this will atomic-decrement state_
  params->opcode_ = io_params::oc_mutex_suspend;
  params->mutex_state_ = &state_;
  curF->switch_to_fiber(params->parent_fiber_);
}

void fiber_mutex::wake_sleepers()
{
  // wake up any blocked fibers by moving them to the params wake list
  auto params = &tls_io_params;

  fiber_wake_tail_->next_wake_ = params->wake_head_;
  params->wake_head_ = fiber_wake_head_;
  fiber_wake_head_ = 0;
  fiber_wake_tail_ = 0;
}

/////////////////////////////////////////////////

int fiber::num_running_fibers_;
std::mutex fiber::zero_fiber_mutex_;
std::condition_variable fiber::zero_fiber_cond_;
std::atomic<int> fiber::next_fiber_id_;
fiber_mutex fiber::on_one_mutex_;
fiber_pipe *fiber::on_one_pipe_;
#if defined(ANON_RUNTIME_CHECKS)
bool fiber::while_paused_;
#endif

void fiber::initialize()
{
#if defined(ANON_RUNTIME_CHECKS)
  if (on_one_pipe_)
    throw std::runtime_error("fiber::initialize already called");
#endif

  int pipe = io_dispatch::new_command_pipe();
  if (fcntl(pipe, F_SETFL, fcntl(pipe, F_GETFL) | O_NONBLOCK) != 0)
    do_error("fcntl(pipe, F_SETFL, fnctl(pipe, F_GETFL) | O_NONBLOCK)");
  on_one_pipe_ = new fiber_pipe(pipe, fiber_pipe::unix_domain);
}

void fiber::terminate()
{
  if (on_one_pipe_ != 0)
  {
    delete on_one_pipe_;
    on_one_pipe_ = 0;
  }
  io_dispatch::remove_task(io_params::next_pipe_sweep_);
}

void fiber::in_fiber_start()
{
  if (!running_)
    anon_log_error("calling fiber::start on an unrunable fiber");
  else
  {
    auto params = &tls_io_params;
    auto cf = params->current_fiber_;
    auto p = params->parent_fiber_;
    if (cf)
      params->parent_fiber_ = cf;
    params->wake_all(this);
    params->parent_fiber_ = p;
  }
}

void fiber::stop_fiber()
{
  auto params = &tls_io_params;
  auto f = params->current_fiber_;
#if defined(ANON_RUNTIME_CHECKS)
  if (while_paused_)
  {
    f->running_ = false;
    f->stop_condition_.notify_all();
  }
  else
#endif
  {
    fiber_lock lock(f->stop_mutex_);
    f->running_ = false;
    f->stop_condition_.notify_all();
  }

  // note that we can come back from the lock on a different
  // os thread than the one that we were originally called on
  // so we need to reset params here.
  params = &tls_io_params;

  params->opcode_ = io_params::oc_exit_fiber;
  f->switch_to_fiber(params->parent_fiber_);
}

int fiber::get_current_fiber_id()
{
  auto params = &tls_io_params;
  return params->current_fiber_ ? params->current_fiber_->fiber_id_ : 0;
}

int get_current_fiber_id()
{
  return fiber::get_current_fiber_id();
}

void *fiber::get_current_fiber()
{
  return tls_io_params.current_fiber_;
}

void *get_current_fiber()
{
  return fiber::get_current_fiber();
}

void fiber::msleep(int milliseconds)
{
  tls_io_params.msleep(milliseconds);
}

/////////////////////////////////////////////////

const struct timespec fiber_pipe::forever = {std::numeric_limits<time_t>::max(), 1000000000 - 1};
fiber_pipe *fiber_pipe::first_ = 0;
std::mutex fiber_pipe::list_mutex_;
int fiber_pipe::num_net_pipes_;
fiber_mutex fiber_pipe::zero_net_pipes_mutex_;
fiber_cond fiber_pipe::zero_net_pipes_cond_;
std::map<void *, std::unique_ptr<fiber_pipe::sweeper_>> fiber_pipe::sweepers_;

void fiber_pipe::start_sweeping()
{
  io_params::next_pipe_sweep_ = io_dispatch::schedule_task(io_params::sweep_timed_out_pipes, cur_time() + 10);
}

void fiber_pipe::io_avail(const struct epoll_event &event)
{
  if (event.events & EPOLLRDHUP)
    remote_hangup_ = true;
  tls_io_params.wake_all(io_fiber_);
}

size_t fiber_pipe::read(void *buf, size_t count) const
{
  anon::assert_no_locks();
  ssize_t num_bytes_read;
  while (true)
  {
    num_bytes_read = ::read(fd_, buf, count);
    if (num_bytes_read == -1)
    {
      if (remote_hangup_)
      {
#if ANON_LOG_NET_TRAFFIC > 1
        anon_log("read(" << fd_ << ", <ptr>, " << count << ") detected remote hangup");
#endif
        throw fiber_io_error("read(fd_, buf, count) detected remote hangup");
      }
      else if (errno == EAGAIN)
        tls_io_params.sleep_until_data_available(const_cast<fiber_pipe *>(this));
      else
      {
#if ANON_LOG_NET_TRAFFIC > 1
        anon_log("read(" << fd_ << ", <ptr>, " << count << ") failed with errno: " << errno_string());
#endif
        throw fiber_io_error("read(fd_, buf, count)");
      }
    }
    else if (num_bytes_read == 0 && count != 0)
    {
#if ANON_LOG_NET_TRAFFIC > 1
      anon_log("read(" << fd_ << ", <ptr>, " << count << ") returned 0, other end probably closed");
#endif
      throw fiber_io_error("read(fd_, buf, count) returned 0, other end probably closed");
    }
    else
      break;
  }
  return (size_t)num_bytes_read;
}

void fiber_pipe::write(const void *buf, size_t count) const
{
  anon::assert_no_locks();
  size_t total_bytes_written = 0;
  const char *p = (const char *)buf;

  while (total_bytes_written < count)
  {
    if (remote_hangup_)
    {
#if ANON_LOG_NET_TRAFFIC > 1
      anon_log("remote hangup detected on write");
#endif
      throw fiber_io_error("remote hangup detected on write");
    }
    auto bytes_written = ::write(fd_, &p[total_bytes_written], count - total_bytes_written);
    if (bytes_written == -1)
    {
      if (errno == EAGAIN)
        tls_io_params.sleep_until_write_possible(const_cast<fiber_pipe *>(this));
      else
      {
#if ANON_LOG_NET_TRAFFIC > 1
        anon_log("write(" << fd_ << ", <ptr>, " << count - total_bytes_written << ") failed with errno: " << errno_string());
#endif
        throw fiber_io_error("write(fd_, &p[total_bytes_written], count - total_bytes_written)");
      }
    }
    else if (bytes_written == 0 && count != 0)
    {
#if ANON_LOG_NET_TRAFFIC > 1
      anon_log("write(" << fd_ << ", <ptr>, " << count - total_bytes_written << ") returned 0, other end probably closed");
#endif
      throw fiber_io_error("write(fd_, &p[total_bytes_written], count - total_bytes_written)");
    }
    else
      total_bytes_written += bytes_written;
  }
}

/////////////////////////////////////////////////

io_dispatch::scheduled_task io_params::next_pipe_sweep_;

void io_params::wake_all(fiber *first)
{
  auto cf = current_fiber_;
  first->next_wake_ = wake_head_;
  wake_head_ = first;

  while (wake_head_)
  {

    auto wake = wake_head_;
    wake_head_ = wake->next_wake_;
    current_fiber_ = wake;
    parent_fiber_->switch_to_fiber(wake);

    switch (opcode_)
    {

    case oc_read:
    case oc_write:
    {

      auto pipe = io_pipe_;
      int op;
      if (!pipe->attached_)
      {
        op = EPOLL_CTL_ADD;
        pipe->attached_ = true;
      }
      else
        op = EPOLL_CTL_MOD;

      io_dispatch::epoll_ctl(op, pipe->fd_,
                             (opcode_ == oc_read ? EPOLLIN : EPOLLOUT) | EPOLLONESHOT | EPOLLET | EPOLLRDHUP,
                             pipe);
    }
    break;

    case oc_mutex_suspend:
      mutex_state_->fetch_add(-1);
      break;

    case oc_cond_wait:
      cond_mutex_->unlock();
      break;

    case oc_sleep:
      io_dispatch::schedule_task([wake] {
        tls_io_params.wake_all(wake);
      },
                                 cur_time() + sleep_dur_);
      break;

    case oc_exit_fiber:
    {
      if (current_fiber_->auto_free_)
        delete current_fiber_;
      anon::unique_lock<std::mutex> lock(fiber::zero_fiber_mutex_);
      if (--fiber::num_running_fibers_ == 0)
        fiber::zero_fiber_cond_.notify_all();
    }
    break;

    default:
      anon_log_error("unknown io_params opcode (" << opcode_ << ")");
      break;
    }
  }

  current_fiber_ = cf;
}

void io_params::sweep_timed_out_pipes()
{
  io_dispatch::while_paused([] {
#if defined(ANON_RUNTIME_CHECKS)
    fiber::while_paused_ = true;
#endif
    for (auto it = fiber_pipe::sweepers_.begin(); it != fiber_pipe::sweepers_.end(); it++)
      it->second->sweep();
#if defined(ANON_RUNTIME_CHECKS)
    fiber::while_paused_ = false;
#endif

    // since no io threads are running now, we know that there
    // is no way for io to be delivered to any of the waiting pipes,
    // causing the epoll code to return and try to handle that
    // io until we return from this function.  But it is still
    // possible for a non-io thread to create or destroy new
    // fiber_pipes (although in practice that essentially never
    // happens).  To guard against that we go ahead and lock
    // the list mutex while we are figuring out which pipes
    // need to be timed out

    fiber_pipe *timed_out = 0;
    {
      std::lock_guard<std::mutex> lock(fiber_pipe::list_mutex_);
      auto ct = cur_time();
      auto pipe = fiber_pipe::first_;
      while (pipe)
      {
        auto next = pipe->next_;
        if (pipe->io_timeout_ < ct)
        {

          // remove from main list
          if (pipe->next_)
            pipe->next_->prev_ = pipe->prev_;
          if (pipe->prev_)
            pipe->prev_->next_ = pipe->next_;
          else
            fiber_pipe::first_ = pipe->next_;

          // add to our to-delete list
          pipe->next_ = timed_out;
          timed_out = pipe;
        }
        pipe = next;
      }
    }

#if defined(ANON_RUNTIME_CHECKS)
    fiber::while_paused_ = true;
#endif
    auto pipe = timed_out;
    auto params = &tls_io_params;
    while (pipe)
    {
      auto next = pipe->next_;
      pipe->next_ = pipe->prev_ = pipe; // so dtor's list removal is a no-op
      params->timeout_expired_ = true;
      params->wake_all(pipe->io_fiber_);
      pipe = next;
    }
#if defined(ANON_RUNTIME_CHECKS)
    fiber::while_paused_ = false;
#endif
  });

  // wake up and sweep every 10 seconds
  bool again = false;
  {
    fiber_lock lock(fiber_pipe::zero_net_pipes_mutex_);
    again = fiber_pipe::num_net_pipes_ > 0;
  }

  if (again)
    next_pipe_sweep_ = io_dispatch::schedule_task(sweep_timed_out_pipes, cur_time() + 10);
  else
    next_pipe_sweep_ = io_dispatch::scheduled_task();
}

void io_params::sleep_until_data_available(fiber_pipe *pipe)
{
  opcode_ = oc_read;
  io_pipe_ = pipe;
  pipe->io_fiber_ = current_fiber_;
  if (pipe->max_io_block_time_ > 0)
    pipe->io_timeout_ = cur_time() + pipe->max_io_block_time_;
  current_fiber_->switch_to_fiber(parent_fiber_);
  pipe->io_fiber_ = 0;
  pipe->io_timeout_ = fiber_pipe::forever;
  if (tls_io_params.timeout_expired_)
  {
    tls_io_params.timeout_expired_ = false;
#if ANON_LOG_NET_TRAFFIC > 0
    anon_log("throwing read io timeout for fd: " << pipe->get_fd());
#endif
    throw std::runtime_error("io timeout");
  }
}

void io_params::sleep_until_write_possible(fiber_pipe *pipe)
{
  opcode_ = oc_write;
  io_pipe_ = pipe;
  pipe->io_fiber_ = current_fiber_;
  if (pipe->max_io_block_time_ > 0)
    pipe->io_timeout_ = cur_time() + pipe->max_io_block_time_;
  current_fiber_->switch_to_fiber(parent_fiber_);
  pipe->io_fiber_ = 0;
  pipe->io_timeout_ = fiber_pipe::forever;
  if (tls_io_params.timeout_expired_)
  {
    tls_io_params.timeout_expired_ = false;
#if ANON_LOG_NET_TRAFFIC > 0
    anon_log("throwing write io timeout for fd: " << pipe->get_fd());
#endif
    throw std::runtime_error("io timeout");
  }
}

void io_params::sleep_cur_until_write_possible(fiber_pipe *pipe)
{
  tls_io_params.sleep_until_write_possible(pipe);
}

void io_params::msleep(int milliseconds)
{
  opcode_ = oc_sleep;
  sleep_dur_.tv_sec = milliseconds / 1000;
  sleep_dur_.tv_nsec = (milliseconds - sleep_dur_.tv_sec * 1000) * 1000000;
  current_fiber_->switch_to_fiber(parent_fiber_);
}
