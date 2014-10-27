
#pragma once

class   fiber;
struct  fiber_mutex;
struct  fiber_lock;
struct  fiber_cond;
class   fiber_pipe;
struct  io_params;

struct fiber_cond
{
  fiber_cond()
    : wake_head_(0)
  {}

  void wait(fiber_lock& lock);
  void notify_one();
  void notify_all();

private:
  fiber*	wake_head_;
};

struct fiber_mutex
{
  fiber_mutex()
    : state_(0)
    , fiber_wake_head_(0)
    , fiber_wake_tail_(0)
  {}

  ~fiber_mutex()
  {
    if (state_)
      anon_trace_error("destructing fiber_mutex " << this << " while locked (state_ = " << state_ << ")");
  }

  void lock();
  void unlock();

private:
  friend class fiber;
  
  void spin_lock()
  {
    // both locks the mutex (sets state_ to >= 1) and blocks any other thread
    // from manipulating the wake list -- implying that any attempt to call lock
    // (or spin_lock) while you have this locked will cause that thread to spin
    // until you call spin_unlock.
    // But this has two possible good points.  1) You will not be suspended when
    // you call this, and 2) there will be no fibers/threads that get
    // added to the wake list while you have this locked, so spin_unlock only has
    // to decrement state_ back to zero.

    while (true) {
      int expected = 0;
      if (std::atomic_compare_exchange_weak(&state_, &expected, 2))
        break;
    }
  }

  // Call this iff you have locked with spin_lock!
  void spin_unlock()
  {
    while (true) {
      int expected = 2;
      if (std::atomic_compare_exchange_weak(&state_, &expected, 0))
        break;
    }
	}
	
	void sleep_till_woken();
	void wake_seepers();

  std::atomic_int state_;
	fiber*          fiber_wake_head_;
	fiber*          fiber_wake_tail_;
};

struct fiber_lock
{
  fiber_lock(fiber_mutex& mutex)
    : mutex_(mutex)
  {
    mutex_.lock();
  }

  ~fiber_lock()
  {
    mutex_.unlock();
  }

private:
  friend struct fiber_cond;
  fiber_mutex&  mutex_;
};

class fiber
{
public:
  fiber(bool convert_calling_thread_to_fiber = false);
  ~fiber();

  template<typename FncType>
  fiber& start(FncType fnc);

  void switch_to_fiber(fiber* target)
  {
    fiber_->switch_to_fiber(target->fiber_);
  }

  void set_io_record(io_record* rec)
  {
    io_rec_ = rec;
  }

  io_record* get_io_record()
  {
    return io_rec_;
  }

  void terminate();
  void join();

  void set_timestamp()
  {
    if (clock_gettime(CLOCK_MONOTONIC, &timestamp_) != 0)
      do_error("clock_gettime(CLOCK_MONOTONIC, &timestamp_)");
  }

private:
  friend bool do_post_switch_params(io_params* this_params, fiber* worker_fiber);
  friend async_io_action get_next_async_io(fiber*& worker_fiber);
  friend struct fiber_mutex;
  friend struct fiber_cond;
  friend struct io_record;

  bool cancel_async_io();

  void in_thread_start(void (*proc)( void* ), void *proc_param);

  void exit();

  void signal_termination()
  {
    // first block any fiber attempting to lock stop_mutex_ (which happens
    // when they are calling join).  This lock is done in a way that causes
    // us, and them, to spin forever waiting for the other side to let go.
    // Neither side is suspended as a part of this call.
    stop_mutex_.spin_lock();

    // set stopped_ true so fibers calling join see this fiber as stopped.
    stopped_ = true;

    // this handles the more common case of a fiber calling join before we got to
    // to signal_termination.  In that case they made it to wait and got suspended.
    // So this schedules those fibers to run again after we return from signal_termination
		terminated_condition_.notify_all();

    // let any fibers calling join succeed in locking
    stopped_mutex_.spin_unlock();
  }

  friend void handle_read_write_opcode(io_record *next_io_rec, fiber* worker_fiber, bool is_read);

  bool            running_;
  fiber_mutex     stop_mutex_;
  fiber_cond      terminated_condition_;
  bool            stopped_;
  bool            stop_;
  io_record*      io_rec_;
  fiber*          next_wake_;
  struct timespec timestamp_;
  fiber*          prev_timeout_;
  fiber*          next_timeout_;
  bool            is_timeout_node_;
  long            timeout_threshold_;
};


class fiber_pipe : public io_dispatch::handler
{
public:
	enum pipe_sock_t	{
		unix_domain = 0,
		network = 1
	};

  fiber_pipe(io_dispatch& io_d, int socket_fd, pipe_sock_t socket_type)
    : fd_(socet_fd),
      socket_type_(socket_type),
      io_fiber_(0)
  {
    io_d.epoll_ctl(EPOLL_CTL_ADD,fd_,0,this);
  }
  
  ~fiber_pipe()
  {
    if (socket_type_ == network)
      ::shutdown(fd_, 2/*both*/);
    close(fd_);
  }
  
  virtual void io_avail(io_dispatch& io_d, const struct epoll_event& event);

  //int read_and_receive_fd(char* buff, int len, int& fd);
  //void write_and_send_fd(const char* buff, int len, int fd);

  int get_fd() const
  {
    return fd_;
  }

  socket_type get_socket_type() const
  {
    return socket_type_;
  }

  int read(char* buff, int len);
  void write(const char* buff, int len);

private:
  int         fd_;
  socket_type socket_type_;
  fiber*      io_fiber_;
};

struct io_params
{
  enum op_code : char
  {
    read = 0,
    write = 1,
    mutex_suspend = 2,
    cond_wait = 3
  };

  io_params()
    : worker_fiber_(0)
    , wake_head_(0)
    , opcode_((op_code)0)
    , iod_fiber_(true/*convertToFiber*/)
  {}
  
  void wake_all(fiber* first);
  void sleep_until_data_available(fiber_pipe* pipe);
  void sleep_until_write_possible(fiber_pipe* pipe);

  fiber*            current_fiber;
  fiber*            wake_head_;
  op_code           opcode_;
  fiber_pipe*       io_pipe_;
  fiber             iod_fiber_;
	std::atomic_int*  mutex_state_;
	fiber_mutex*      cond_mutex_;
};

void exit_fiber();


template<typename Fn>
void fiber_start_proc(void* param)
{
  Fn* fnc = (Fn*)param;
  try {
    (*fnc)();
  }
  catch(std::exception& ex)
  {
    anon_log_error("uncaught exception in fiber, what() = " << ex.what());
  }
  catch(...)
  {
    anon_log_error("uncaught exception in fiber");
  }
  delete fnc;
  exit_fiber();
}

template<typename Fn>
fiber& fiber::start(io_dispatch& io_d, Fn fnc)
{
  io_d.on_one([this]{fiber_start_proc<Fn>(new Fn(fnc));});
	return *this;
}

inline void fiber_mutex::lock()
{
  while (true) {
    switch(state_.fetch_add(1)) {
    
      case 0:
        // we got the lock, state_ is now 1
        return;

      case 1:
        // state_ is now 2.  someone else has it locked,
        // but no one else is manipulating the wake list.
        sleep_till_woken();
        break;

			default:
        // someone has it locked, and someone else is
        // manipulating the wake list. spin and try again
        state_.fetch_add(-1);
        break;
		}
	}
}

inline void fiber_mutex::unlock()
{
  // Increment state_ to try to grab the right to manipulate the wake list.
  // We locked this mutex, and when we did we set state_ to 1.  Other
  // threads may be concurrently trying to lock the mutex right now
  // and so might temporarily have state_ set to something other than 1.
  // Here we wait for them to finish whatever they are doing and only set
  // state_ to 2 at a point in time when it was 1.  Once we set it to 2 no
  // other fiber calling lock (which increments state_) will do anything
  // other than decrement it back and then try to increment it again.
  while (true) {
    int expected = 1;
    if (std::atomic_compare_exchange_weak(&state_, &expected, 2))
      break;
  }

  // if there are any fibers that hit case '1' in ::lock, then
  // schedule them to start running again
  if (fiber_wake_head_)
    wake_sleepers();

	// release both our lock on the mutex and ownership of manipulating the wake
	// list (2) Again, although we know state_ was == 2 above when we got through
	// the while loop at the start of this routine, it might be > 2 now, meaning
	// some other fiber has incremented it when it attempted to lock it.  In that
	// case we can't simply decrement state_ by 2 here because that might leave
	// it set to 1 (for example, if it had been 3), looking to that other fiber
	// like it was locked.  If yet another fiber attempted to lock it at that point
	// before the first other thread decremented it back to 0 it would look locked
	// to that third thread, causing it to add itself to the wake list.  That is
	// wrong because no one owns the lock at that point.  Here we only set state_
	// back to zero at a point in time when it is equal to 2.
  while (true) {
    int expected = 2;
    if (std::atomic_compare_exchange_weak(&state_, &expected, 0))
      break;
  }
}


