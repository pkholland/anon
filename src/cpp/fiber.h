
#pragma once

#include "io_dispatch.h"
#include "log.h"
#include <ucontext.h>
#include <vector>
#include <atomic>
#include <exception>
#include <sys/socket.h>
#include <unistd.h>

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
      anon_log_error("destructing fiber_mutex " << this << " while locked (state_ = " << state_ << ")");
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
	void wake_sleepers();

  std::atomic<int>  state_;
	fiber*            fiber_wake_head_;
	fiber*            fiber_wake_tail_;
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
  // singleton call to tell the fiber code
  // what io_dispatch you are using.
  static void attach(io_dispatch& io_d);
  
  // run the given 'fn' in this fiber.  If you pass 'detached' true
  // then the code will automatically (attempt to) call 'delete' on
  // this fiber after 'fn' returns.
  template<typename Fn>
  fiber(Fn fn, size_t stack_size=1024*1024, bool detached=false)
    : stack_(stack_size),
      detached_(detached),
      running_(true),
      fiber_id_(++next_fiber_id_)
  {
    if (!detached_) {
      std::unique_lock<std::mutex> lock(zero_fiber_mutex_);
      ++num_running_fibers_;
    }
    getcontext(&ucontext_);
    ucontext_.uc_stack.ss_sp = &stack_[0];
    ucontext_.uc_stack.ss_size = stack_size;
    ucontext_.uc_link = NULL;
    auto sm = new start_mediator<Fn>(fn);
    int p1 = (int)((uint64_t)sm);
    int p2 = (int)(((uint64_t)sm) >> 32);
    makecontext(&ucontext_, (void (*)())&fiber::start_fiber, 2, p1, p2);
    in_fiber_start();
  }
  
  // note! calling join can switch threads -- that is, you can
  // come back from join on a different os thread than the one
  // you called it on.
  void join()
  {
    fiber_lock lock(stop_mutex_);
    while (running_)
      stop_condition_.wait(lock);
  }

  // helper to execute 'fn' in a newly allocated fiber, running
  // on one of the io threads of the io_dispatch passed to attach.
  // The fiber will automatically be deleted when 'fn' returns.
  template<typename Fn>
  static void run_in_fiber(Fn fn, size_t stack_size=1024*1024)
  {
    if (!io_d_)
      do_error("must call fiber::attach prior to fiber::run_in_fiber");
      
    {
      std::unique_lock<std::mutex> lock(zero_fiber_mutex_);
      ++num_running_fibers_;
    }
    io_d_->on_one([fn, stack_size]{new fiber(fn,stack_size,true/*detached*/);});
  }
  
  static void wait_for_zero_fibers()
  {
    std::unique_lock<std::mutex> lock(zero_fiber_mutex_);
    while (num_running_fibers_ != 0)
      zero_fiber_cond_.wait(lock);
  }
  
  static int get_current_fiber_id();
  int get_fiber_id()
  {
    return fiber_id_;
  }

private:
  // a 'parent' -like fiber, illegal to call 'start' on one of these
  // this is the kind that live in io_params.iod_fiber_
  fiber()
    : detached_(false),
      running_(false)
  {
    getcontext(&ucontext_);
  }

  struct start_mediator_
  {
  public:
    virtual ~start_mediator_() {}
    virtual void exec() = 0;
  };
  
  template<typename Fn>
  struct start_mediator : public start_mediator_
  {
  public:
    start_mediator(Fn fn) : fn_(fn) {}
    virtual void exec() { fn_(); }
    Fn fn_;
  };
  
  static void start_fiber(int p1, int p2)
  {
    start_mediator_ *sm = (start_mediator_*)(((uint64_t)p1 & 0x0ffffffff) + (((uint64_t)p2) << 32));
    try {
      sm->exec();
    }
    catch(std::exception& ex)
    {
      anon_log_error("uncaught exception in fiber, what() = " << ex.what());
    }
    catch(...)
    {
      anon_log_error("uncaught exception in fiber");
    }
    delete sm;
    stop_fiber();
  }
  
  void switch_to_fiber(fiber* target)
  {
    swapcontext(&ucontext_, &target->ucontext_);
  }
  
  void in_fiber_start();
  static void stop_fiber();

  friend struct fiber_mutex;
  friend struct fiber_cond;
  friend struct io_params;
  friend class fiber_pipe;

  bool              running_;
  bool              detached_;
  fiber_mutex       stop_mutex_;
  fiber_cond        stop_condition_;
  fiber*            next_wake_;
  std::vector<char> stack_;
  ucontext_t        ucontext_;
  int               fiber_id_;
  
  static io_dispatch* io_d_;
  static int num_running_fibers_;
  static std::mutex zero_fiber_mutex_;
  static std::condition_variable zero_fiber_cond_;
  static std::atomic<int> next_fiber_id_;
};

extern int get_current_fiber_id();

////////////////////////////////////////////////////////////////////////

class fiber_pipe : public io_dispatch::handler
{
public:
	enum pipe_sock_t	{
		unix_domain = 0,
		network = 1
	};

  fiber_pipe(int socket_fd, pipe_sock_t socket_type)
    : fd_(socket_fd),
      socket_type_(socket_type),
      io_fiber_(0)
  {
    fiber::io_d_->epoll_ctl(EPOLL_CTL_ADD,fd_,0,this);
  }
  
  ~fiber_pipe()
  {
    if (fd_ != -1) {
      if (socket_type_ == network)
        shutdown(fd_, 2/*both*/);
      close(fd_);
    }
  }
  
  virtual void io_avail(io_dispatch& io_d, const struct epoll_event& event);

  //int read_and_receive_fd(char* buff, int len, int& fd);
  //void write_and_send_fd(const char* buff, int len, int fd);

  int get_fd() const
  {
    return fd_;
  }

  pipe_sock_t get_socket_type() const
  {
    return socket_type_;
  }
  
  int release()
  {
    int ret = fd_;
    if (fd_ != -1) {
      fiber::io_d_->epoll_ctl(EPOLL_CTL_DEL,fd_,0,this);
      fd_ = -1;
    }
    return ret;
  }

  size_t read(void* buff, size_t len);
  void write(const void* buff, size_t len);

private:
  // fiber_pipe's are neither movable, nor copyable.
  // the address of the pipe is regestered in epoll
  fiber_pipe(const fiber_pipe&);
  fiber_pipe(fiber_pipe&&);
  
  friend struct io_params;
  
  int         fd_;
  pipe_sock_t socket_type_;
  fiber*      io_fiber_;
};

////////////////////////////////////////////////////////////////

struct io_params
{
  enum op_code : char
  {
    oc_read,
    oc_write,
    oc_mutex_suspend,
    oc_cond_wait,
    oc_exit_fiber
  };

  io_params()
    : current_fiber_(0),
      parent_fiber_(&iod_fiber_),
      wake_head_(0)
  {}
  
  void wake_all(fiber* first);
  void sleep_until_data_available(fiber_pipe* pipe);
  void sleep_until_write_possible(fiber_pipe* pipe);
  void exit_fiber();

  fiber*            current_fiber_;
  fiber*            parent_fiber_;
  fiber*            wake_head_;
  op_code           opcode_;
  fiber_pipe*       io_pipe_;
  fiber             iod_fiber_;
  std::atomic_int*  mutex_state_;
  fiber_mutex*      cond_mutex_;
};


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


