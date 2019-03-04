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

#pragma once

#include "io_dispatch.h"
#include "log.h"
#include "lock_checker.h"
#include "pipe.h"
#include <ucontext.h>
#include <vector>
#include <atomic>
#include <exception>
#include <sys/socket.h>
#include <unistd.h>

class fiber;
struct fiber_lock;
class fiber_pipe;
struct io_params;

struct fiber_cond
{
  fiber_cond()
      : wake_head_(0)
  {
  }

  void wait(fiber_lock &lock);
  void notify_one();
  void notify_all();

private:
  fiber *wake_head_;
};

struct fiber_mutex
{
  fiber_mutex()
      : state_(0), fiber_wake_head_(0), fiber_wake_tail_(0)
  {
  }

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

    while (true)
    {
      int expected = 0;
      if (std::atomic_compare_exchange_weak(&state_, &expected, 2))
        break;
    }
  }

  // Call this iff you have locked with spin_lock!
  void spin_unlock()
  {
    while (true)
    {
      int expected = 2;
      if (std::atomic_compare_exchange_weak(&state_, &expected, 0))
        break;
    }
  }

  void sleep_till_woken();
  void wake_sleepers();

  std::atomic<int> state_;
  fiber *fiber_wake_head_;
  fiber *fiber_wake_tail_;
};

struct fiber_lock
{
  fiber_lock(fiber_mutex &mutex)
      : mutex_(mutex),
        is_locked_(true)
  {
    mutex_.lock();
  }

  ~fiber_lock()
  {
    if (is_locked_)
      mutex_.unlock();
  }

  void lock()
  {
    if (!is_locked_)
    {
      mutex_.lock();
      is_locked_ = true;
    }
  }

  void unlock()
  {
    if (is_locked_)
    {
      mutex_.unlock();
      is_locked_ = false;
    }
  }

private:
  friend struct fiber_cond;
  fiber_mutex &mutex_;
  bool is_locked_;
};

class fiber
{
public:
  // singleton call to initialize/terminate the fiber code

  static void initialize(); // must be called _after_ io_dispatch::start()
  static void terminate();  // must be called _after_ io_dispatch::join()

  enum
  {
    k_default_stack_size = 32 * 1024 - 256
  };

  // run the given 'fn' in this fiber.  If you pass 'detached' true
  // then the code will automatically (attempt to) call 'delete' on
  // this fiber after 'fn' returns.
  template <typename Fn>
  fiber(const Fn &fn, size_t stack_size = k_default_stack_size, bool auto_free = false,
        const char *fiber_name = "unknown1")
      : auto_free_(auto_free),
        running_(true),
        stack_(::operator new(stack_size)),
        fiber_id_(++next_fiber_id_),
        timout_pipe_(0)
#if defined(ANON_RUNTIME_CHECKS)
        ,
        stack_size_(stack_size),
        fiber_name_(fiber_name)
#endif
  {
    if (!auto_free_)
    {
      anon::unique_lock<std::mutex> lock(zero_fiber_mutex_);
      ++num_running_fibers_;
    }
    getcontext(&ucontext_);
#if defined(ANON_RUNTIME_CHECKS)
    int *s = (int *)stack_;
    int *se = s + (stack_size_ / sizeof(int));
    while (s < se)
      *s++ = 0xbaadf00d;
#endif
    ucontext_.uc_stack.ss_sp = stack_;
    ucontext_.uc_stack.ss_size = stack_size;
    ucontext_.uc_link = NULL;
    auto sm = new start_mediator<Fn>(fn);
    int p1 = (int)((uint64_t)sm);
    int p2 = (int)(((uint64_t)sm) >> 32);
    makecontext(&ucontext_, (void (*)()) & fiber::start_fiber, 2, p1, p2);
    in_fiber_start();
  }

  ~fiber()
  {
#if defined(ANON_RUNTIME_CHECKS)
    if (stack_)
    {
      int *s = (int *)stack_;
      int *se = s + (stack_size_ / sizeof(int));
      while (s < se && *s == 0xbaadf00d)
        ++s;
      anon_log("fiber \"" << fiber_name_ << "\" consumed " << (char *)se - (char *)s << " bytes of stackspace, leaving " << (char *)s - (char *)stack_ << " untouched");
    }
#endif
    ::operator delete(stack_);
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
  template <typename Fn>
  static void run_in_fiber(const Fn &fn, size_t stack_size = k_default_stack_size, const char *fiber_name = "unknown2")
  {
#if defined(ANON_RUNTIME_CHECKS)
    if (!on_one_pipe_)
      do_error("must call fiber::initialize prior to fiber::run_in_fiber");
#endif

    {
      anon::unique_lock<std::mutex> lock(zero_fiber_mutex_);
      ++num_running_fibers_;
    }
    auto oo_fn = [fn, stack_size, fiber_name] { new fiber(fn, stack_size, true /*auto_free*/, fiber_name); };
    if (get_current_fiber_id() == 0)
      io_dispatch::on_one(oo_fn);
    else
    {
      char buf[io_dispatch::k_oo_command_buf_size];
      io_dispatch::on_one_command(oo_fn, buf);
      write_on_one_command(buf);
    }
  }

  // note that this is an os thread blocking wait, and
  // should NOT be called from a fiber.  Doing so will
  // dead-lock the calling fiber since the running fiber
  // count can't go to zero while the calling fiber is
  // continuing to run, waiting for it to go to zero.
  static void wait_for_zero_fibers()
  {
    anon::unique_lock<std::mutex> lock(zero_fiber_mutex_);
    while (num_running_fibers_ != 0)
      zero_fiber_cond_.wait(lock);
  }

  static int get_current_fiber_id();
  int get_fiber_id()
  {
    return fiber_id_;
  }

  static void *get_current_fiber();

  static void msleep(int milliseconds);

private:
  // a 'parent' -like fiber, illegal to call 'start' on one of these
  // this is the kind that live in io_params.iod_fiber_
  fiber()
      : auto_free_(false),
        running_(false),
        stack_(0)
  {
    getcontext(&ucontext_);
  }

  struct start_mediator_
  {
  public:
    virtual ~start_mediator_() {}
    virtual void exec() = 0;
  };

  template <typename Fn>
  struct start_mediator : public start_mediator_
  {
  public:
    start_mediator(const Fn &fn) : fn_(fn) {}
    virtual void exec() { fn_(); }
    Fn fn_;
  };

  static void start_fiber(int p1, int p2)
  {
    start_mediator_ *sm = (start_mediator_ *)(((uint64_t)p1 & 0x0ffffffff) + (((uint64_t)p2) << 32));
    try
    {
      sm->exec();
    }
    catch (std::exception &ex)
    {
      anon_log_error("uncaught exception in fiber, what() = " << ex.what());
    }
    catch (...)
    {
      anon_log_error("uncaught exception in fiber");
    }
    delete sm;
    stop_fiber();
  }

  void switch_to_fiber(fiber *target)
  {
    swapcontext(&ucontext_, &target->ucontext_);
  }

  static void write_on_one_command(char (&buf)[io_dispatch::k_oo_command_buf_size]);

  void in_fiber_start();
  static void stop_fiber();

  friend struct fiber_mutex;
  friend struct fiber_cond;
  friend struct io_params;
  friend class fiber_pipe;

  bool auto_free_;
  bool running_;
  fiber_mutex stop_mutex_;
  fiber_cond stop_condition_;
  fiber *next_wake_;
  void *stack_;
  ucontext_t ucontext_;
  int fiber_id_;
  fiber_pipe *timout_pipe_;

#if defined(ANON_RUNTIME_CHECKS)
  size_t stack_size_;
  const char *fiber_name_;
#endif

  static int num_running_fibers_;
  static std::mutex zero_fiber_mutex_;
  static std::condition_variable zero_fiber_cond_;
  static std::atomic<int> next_fiber_id_;
  static fiber_mutex on_one_mutex_;
  static fiber_pipe *on_one_pipe_;

  friend void sweep_timed_out_pipes();

// there are a few places where the runtime checks are
// too agressive, and the lock checking performed cannot
// be warning about something bad.  The primary issue being
// reported is that locking a fiber mutex can cause you
// to come back from the lock in a different os thread
// (although obviously in the same fiber).  So it would
// be a disaster if there were an std mutex locked in that
// os thread at the time this thread switch occurred.
// But this os switch is not possible on the fiber locks
// that are protected with this "while_paused_" logic in
// the case where "while_paused_" is true.  We test for,
// and skip the fiber lock in these cases just to avoid
// the runtime assertion.
#if defined(ANON_RUNTIME_CHECKS)
  static bool while_paused_;
#endif
};

extern int get_current_fiber_id();
extern void *get_current_fiber();

////////////////////////////////////////////////////////////////////////

class fiber_pipe : public io_dispatch::handler, public pipe_t
{
public:
  enum pipe_sock_t
  {
    unix_domain = 0,
    network = 1
  };

  fiber_pipe(int socket_fd, pipe_sock_t socket_type)
      : fd_(socket_fd),
        socket_type_(socket_type),
        io_fiber_(0),
        max_io_block_time_(0),
        io_timeout_(forever),
        attached_(false),
        remote_hangup_(false)
  {
    bool start_sweep = false;
    if (socket_type == network)
    {
      fiber_lock lock(zero_net_pipes_mutex_);
      start_sweep = num_net_pipes_ == 0;
      ++num_net_pipes_;
    }

    if (start_sweep)
      start_sweeping();

    std::lock_guard<std::mutex> lock(list_mutex_);
    next_ = first_;
    if (next_)
      next_->prev_ = this;
    prev_ = 0;
    first_ = this;
  }

  virtual ~fiber_pipe()
  {
    if (fd_ != -1)
    {
      if (socket_type_ == network)
      {
        shutdown(fd_, 2 /*both*/);
#if defined(ANON_RUNTIME_CHECKS)
        if (fiber::while_paused_)
        {
          if (--num_net_pipes_ == 0)
            zero_net_pipes_cond_.notify_all();
        }
        else
#endif
        {
          fiber_lock lock(zero_net_pipes_mutex_);
          if (--num_net_pipes_ == 0)
            zero_net_pipes_cond_.notify_all();
        }
      }
      if (close(fd_) != 0)
      {
        anon_log("close(" << fd_ << ") failed with errno: " << errno);
      }
    }
    std::lock_guard<std::mutex> lock(list_mutex_);
    if (next_)
      next_->prev_ = prev_;
    if (prev_)
      prev_->next_ = next_;
    else
      first_ = next_;
  }

  virtual void io_avail(const struct epoll_event &event);

  virtual void limit_io_block_time(int seconds)
  {
    max_io_block_time_ = seconds;
  }

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
    if (fd_ != -1 && attached_)
    {
      io_dispatch::epoll_ctl(EPOLL_CTL_DEL, fd_, 0, this);
      attached_ = false;
    }
    fd_ = -1;
    return ret;
  }

  virtual size_t read(void *buff, size_t len) const;
  virtual void write(const void *buff, size_t len) const;

  static void wait_for_zero_net_pipes()
  {
    fiber_lock lock(zero_net_pipes_mutex_);
    while (num_net_pipes_)
      zero_net_pipes_cond_.wait(lock);
  }

  template <typename Fn>
  static void register_idle_socket_sweep(void *key, Fn f)
  {
    fiber_lock lock(zero_net_pipes_mutex_);
    sweepers_.insert(std::make_pair(key, std::unique_ptr<sweeper_>(new sweeper<Fn>(f))));
  }

  static void remove_idle_socket_sweep(void *key)
  {
    fiber_lock lock(zero_net_pipes_mutex_);
    sweepers_.erase(key);
  }

private:
  // fiber_pipe's are neither movable, nor copyable.
  // the address of the pipe is regestered in epoll
  fiber_pipe(const fiber_pipe &);
  fiber_pipe(fiber_pipe &&);

  static void start_sweeping();

  friend struct io_params;

  int fd_;
  pipe_sock_t socket_type_;
  fiber *io_fiber_;
  int max_io_block_time_;
  struct timespec io_timeout_;
  bool attached_;
  bool remote_hangup_;

  fiber_pipe *next_;
  fiber_pipe *prev_;
  static fiber_pipe *first_;
  static std::mutex list_mutex_;

  static int num_net_pipes_;
  static fiber_mutex zero_net_pipes_mutex_;
  static fiber_cond zero_net_pipes_cond_;

  static const struct timespec forever;

  struct sweeper_
  {
    virtual ~sweeper_() {}
    virtual void sweep() = 0;
  };

  template <typename Fn>
  struct sweeper : public sweeper_
  {
    sweeper(Fn f) : f_(f) {}
    virtual void sweep() { f_(); }
    Fn f_;
  };

  static std::map<void *, std::unique_ptr<sweeper_>> sweepers_;
};

/*
  fiber_io_error exceptions thrown in the context
  of endpoint_cluster::with_connected_pipe have some
  special processing.  In this context a fiber_io_error
  with backoff_ set to false is understood to mean a
  case where the socket is in an error state (typically
  the other side has closed) and we should close our side.
  A fiber_io_error with backoff_ set to true is an indication
  that the other side is busy and we should delay further
  attempts to talk to it by at least backoff_seconds_ seconds.
  In this case the close_socket_hint_ hints at whether
  we should also close our side of the socket (like the
  backoff_ = false case) and then reconnect when we attempt
  to communicate again, or whether we should leave this socket
  connected and just reuse this one.
*/
class fiber_io_error : public std::runtime_error
{
public:
  fiber_io_error(const char *what_arg)
      : std::runtime_error(what_arg)
  {
  }

  fiber_io_error(const std::string &what_arg)
      : std::runtime_error(what_arg)
  {
  }
};

////////////////////////////////////////////////////////////////

inline void fiber::write_on_one_command(char (&buf)[io_dispatch::k_oo_command_buf_size])
{
  fiber_lock lock(on_one_mutex_);
  on_one_pipe_->write(&buf, sizeof(buf));
}

////////////////////////////////////////////////////////////////

struct io_params
{
  enum op_code : char
  {
    oc_read,
    oc_write,
    oc_mutex_suspend,
    oc_cond_wait,
    oc_sleep,
    oc_exit_fiber
  };

  io_params()
      : current_fiber_(0),
        parent_fiber_(&iod_fiber_),
        wake_head_(0),
        timeout_expired_(0)
  {
  }

  void wake_all(fiber *first);
  void sleep_until_data_available(fiber_pipe *pipe);
  void sleep_until_write_possible(fiber_pipe *pipe);
  void msleep(int milliseconds);
  void exit_fiber();

  static void sleep_cur_until_write_possible(fiber_pipe *pipe);

  fiber *current_fiber_;
  fiber *parent_fiber_;
  fiber *wake_head_;
  op_code opcode_;
  fiber_pipe *io_pipe_;
  fiber iod_fiber_;
  std::atomic_int *mutex_state_;
  fiber_mutex *cond_mutex_;
  struct timespec sleep_dur_;
  bool timeout_expired_;

  static io_dispatch::scheduled_task next_pipe_sweep_;
  static void sweep_timed_out_pipes();
};

inline void fiber_mutex::lock()
{
  anon::assert_no_locks();

  int num_spins = 1;
  while (true)
  {
    switch (state_.fetch_add(1))
    {

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
      num_spins *= 2;
      if (num_spins > 100000)
        num_spins = 100000;
      for (auto i = 0; i < num_spins; i++)
        asm("");
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
  while (true)
  {
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
  while (true)
  {
    int expected = 2;
    if (std::atomic_compare_exchange_weak(&state_, &expected, 0))
      break;
  }
}
