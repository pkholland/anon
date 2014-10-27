
#include "fiber.h"

thread_local io_params  tls_io_params;

void fiber_mutex::sleep_till_woken()
{
  io_params* params = &tls_io_params;
  
  // Add this fiber to this mutex's wakeList and then suspend the fiber.
  // Note that the decrement of state_ happens after we have switched to
  // the iod fiber.
  fiber* curF = params->current_fiber_;
  if (!fiber_wake_tail_)
    fiber_wake_tail_ = curF;
  curF->next_wake_ = fiber_wake_head_;
  fiber_wake_head_ = curF;

  // this will atomic-decrement state_
  params->opcode_ = io_params::mutex_suspend;
  params->mutex_state_ = &state_;
  curF->switch_to_fiber(&params->iod_fiber_);
}

void fiber_mutex::wake_sleepers()
{
  // wake up any blocked fibers by moving them to the params wake list
  io_params* params = &tls_io_params;

  fiber_wake_tail_->next_wake_ = params->wake_head_;
  params->wake_head_ = fiber_wake_head_;
  fiber_wake_head_ = 0;
  fiber_wake_tail_ = 0;
}

/////////////////////////////////////////////////

void fiber_pipe::io_avail(io_dispatch& io_d, const struct epoll_event& event)
{
  tls_io_params.wake_all(io_fiber_);
}

ssize_t fiber_pipe::read(void *buf, size_t count)
{
  ssize_t num_bytes_read;
  while (true) {
    num_bytes_read = ::read(fd_, buf, count);
    if (num_bytes_read == -1) {
			if (errno == EAGAIN)
				tls_io_params.sleep_until_data_available(this);
      else
        do_error("read(" << fd_ << ", ptr, " << count << ")");
    } else if (num_bytes_read == 0 && count != 0)
       do_error("read(" << fd_ << ", ptr, " << count << ")");
		else
			break;
	}
	return num_bytes_read;
}

void fiber_pipe::write(const void *buf, size_t count)
{
  size_t total_bytes_written = 0;
  const char* p = (const char*)buf;
	
  while (total_bytes_written < count) {
    auto bytes_written = ::send(fd_, &p[totalBytesWritten], count - total_bytes_written, MSG_NOSIGNAL);
    if (bytes_written == -1) {
      if (errno == EAGAIN)
        tls_io_params.sleep_until_write_possible(this);
      else
        do_error("send(" << fd_ << ", ptr, " << count - total_bytes_written << ", MSG_NOSIGNAL)");
    } else if (bytes_written == 0 && count != 0)
       do_error("send(" << fd_ << ", ptr, " << count - total_bytes_written << ", MSG_NOSIGNAL)");
    else
      total_bytes_written += bytes_written;
	}
}


/////////////////////////////////////////////////

void io_params::wake_all(fiber* first)
{
  first->next_wake_ = wake_head_;
  wake_head_ = first;

  while (wake_head_) {
  
    auto wake = wake_head_;
    wake_head_ = wake->next_wake_;
    current_fiber = wake;
    iod_fiber_.switch_to_fiber(wake);
      
     switch(opcode_) {

        case read:
        case write:
           io_pipe_->io_d_.epoll_ctl(EPOLL_CTL_MOD, io_pipe_->fd_,
                                    (opcode_ == read ? EPOLLIN : EPOLLOUT) | EPOLLET,
                                    io_pipe_->io_fiber_);
          break;

        case mutex_suspend:
          mutex_state_->fetch_add(-1);
          break;

        case cond_wait:
          cond_mutex_->unlock();
          break;

        default:
          anon_log_error("unknown io_params opcode (" << (int)params->opcode << ")");
          break;
      }
    }
  }
}

void io_params::sleep_until_data_available(fiber_pipe* pipe)
{
	opcode_ = read;
	io_pipe_ = pipe;
	curren_fiber->switch_to_fiber(&iod_fiber);
}

void io_params::sleep_until_write_possible(fiber_pipe* pipe)
{
	opcode_ = write;
	io_pipe_ = pipe;
	curren_fiber->switch_to_fiber(&iod_fiber);
}


