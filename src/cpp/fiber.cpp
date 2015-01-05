/*
 Copyright (c) 2014 Anon authors, see AUTHORS file.
 
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
#include <fcntl.h>

thread_local io_params  tls_io_params;

// suspend the calling fiber on the fiber_cond's wake list
// then jump back to the parent fiber, telling it to unlock
// the mutex.  When we return (after some other fiber calls
// one of the notify functions), attempt to lock the mutex.
void fiber_cond::wait(fiber_lock& lock)
{
  anon::assert_no_locks();
  
  auto params = &tls_io_params;
  auto f = params->current_fiber_;
  
  f->next_wake_ = 0;
  if (wake_head_) {
    auto last = wake_head_;
    while (last->next_wake_)
      last = last->next_wake_;
    last->next_wake_ = f;
  } else
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
  if (wake_head_) {
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
  if (wake_head_) {
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
  fiber* curF = params->current_fiber_;
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
fiber_pipe* fiber::on_one_pipe_;

void fiber::initialize()
{
  #if defined(ANON_RUNTIME_CHECKS)
  if (on_one_pipe_)
    throw std::runtime_error("fiber::initialize already called");
  #endif
  
  int pipe = io_dispatch::new_command_pipe();
  if (fcntl(pipe, F_SETFL, O_NONBLOCK) != 0)
    do_error("fcntl(pipe, F_SETFL, O_NONBLOCK)");
  on_one_pipe_ = new fiber_pipe(pipe, fiber_pipe::unix_domain);
}

void fiber::terminate()
{
  if (on_one_pipe_ != 0)  {
    delete on_one_pipe_;
    on_one_pipe_ = 0;
  }
}

void fiber::in_fiber_start()
{
  if (!running_)
    anon_log_error("calling fiber::start on an unrunable fiber");
  else {
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

void* fiber::get_current_fiber()
{
  return tls_io_params.current_fiber_;
}

void* get_current_fiber()
{
  return fiber::get_current_fiber();
}

/////////////////////////////////////////////////

void fiber_pipe::io_avail(const struct epoll_event& event)
{
  if (event.events & (EPOLLIN | EPOLLOUT))
    tls_io_params.wake_all(io_fiber_);
}

size_t fiber_pipe::read(void *buf, size_t count)
{
  anon::assert_no_locks();
  ssize_t num_bytes_read;
  while (true) {
    num_bytes_read = ::read(fd_, buf, count);
    if (num_bytes_read == -1) {
			if (errno == EAGAIN)
				tls_io_params.sleep_until_data_available(this);
      else
        do_error("read(" << fd_ << ", <ptr>, " << count << ")");
    } else if (num_bytes_read == 0 && count != 0) {
      #if ANON_LOG_NET_TRAFFIC > 1
      anon_log("read(" << fd_ << ", <ptr>, " << count << ") returned 0");
      #endif
      throw std::runtime_error("read returned 0, other end probably closed");
    }
		else
			break;
	}
	return (size_t)num_bytes_read;
}

void fiber_pipe::write(const void *buf, size_t count)
{
  anon::assert_no_locks();
  size_t total_bytes_written = 0;
  const char* p = (const char*)buf;
	
  while (total_bytes_written < count) {
    auto bytes_written = ::send(fd_, &p[total_bytes_written], count - total_bytes_written, MSG_NOSIGNAL);
    if (bytes_written == -1) {
      if (errno == EAGAIN)
        tls_io_params.sleep_until_write_possible(this);
      else
        do_error("send(" << fd_ << ", <ptr>, " << count - total_bytes_written << ", MSG_NOSIGNAL)");
    } else if (bytes_written == 0 && count != 0) {
      #if ANON_LOG_NET_TRAFFIC > 1
      anon_log("send(" << fd_ << ", <ptr>, " << count - total_bytes_written << ", MSG_NOSIGNAL)");
      #endif
      throw std::runtime_error("send returned 0, other end probably closed");
    }
    else
      total_bytes_written += bytes_written;
	}
}


/////////////////////////////////////////////////

void io_params::wake_all(fiber* first)
{
  auto cf = current_fiber_;
  first->next_wake_ = wake_head_;
  wake_head_ = first;

  while (wake_head_) {
  
    auto wake = wake_head_;
    wake_head_ = wake->next_wake_;
    current_fiber_ = wake;
    parent_fiber_->switch_to_fiber(wake);
      
    switch(opcode_) {

      case oc_read:
      case oc_write:
        io_dispatch::epoll_ctl(EPOLL_CTL_MOD, io_pipe_->fd_,
                                (opcode_ == oc_read ? EPOLLIN : EPOLLOUT) | EPOLLONESHOT | EPOLLET,
                                io_pipe_);
        break;

      case oc_mutex_suspend:
        mutex_state_->fetch_add(-1);
        break;

      case oc_cond_wait:
        cond_mutex_->unlock();
        break;
          
      case oc_exit_fiber: {
        if (current_fiber_->auto_free_)
          delete current_fiber_;
        anon::unique_lock<std::mutex> lock(fiber::zero_fiber_mutex_);
        if (--fiber::num_running_fibers_ == 0)
            fiber::zero_fiber_cond_.notify_all();
      } break;

      default:
        anon_log_error("unknown io_params opcode (" << opcode_ << ")");
        break;

    }
  }
  
  current_fiber_ = cf;
}

void io_params::sleep_until_data_available(fiber_pipe* pipe)
{
	opcode_ = oc_read;
	io_pipe_ = pipe;
	pipe->io_fiber_ = current_fiber_;
	current_fiber_->switch_to_fiber(parent_fiber_);
	pipe->io_fiber_ = 0;
}

void io_params::sleep_until_write_possible(fiber_pipe* pipe)
{
	opcode_ = oc_write;
	io_pipe_ = pipe;
	pipe->io_fiber_ = current_fiber_;
	current_fiber_->switch_to_fiber(parent_fiber_);
	pipe->io_fiber_ = 0;
}

void io_params::sleep_cur_until_write_possible(fiber_pipe* pipe)
{
  tls_io_params.sleep_until_write_possible(pipe);
}


