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

#include "lock_checker.h"
#include "time_utils.h"
#include <functional>
#include <vector>
#include <thread>
#include <mutex>
#include <list>
#include <condition_variable>
#include <map>
#include <atomic>
#include <sys/epoll.h>
#include <string.h>
#include <signal.h>

class io_ctl_handler;

class io_dispatch
{
  io_dispatch();
  ~io_dispatch();

public:
  // 'num_threads' is the total number of threads that will be used
  // to dispatch io events on.  If 'use_this_thread' is true, then
  // this calling thread will be one of those threads.  Regardless,
  // this call will start all threads other than this one.  If
  // use_this_thread is true then you must call start_this_thread
  // after this call to start returns.  This is done to permit
  // further initialization prior to having the calling thread
  // join the pool of io threads.
  //
  // numSigFds is intended to support applications that want
  // to use signal handling from within fibers.  Typically this
  // implies that the signal(s) need to be blocked for all threads
  // and fibers in the app, and since the signal disposition for
  // each thread and fiber is inherited from whatever thread
  // created that fiber, we set the signal disposition to
  // SIG_BLOCK for each signal from firstSig to
  // (firstSig + numSigs - 1).
  static void start(int num_threads, bool use_this_thread,
                    int numSigs = 0, int firstSig = SIGRTMIN);

  static void start_this_thread();

  // starts the sequence of stopping all io threads.  To wait
  // until they have all stopped call join -- but, of course
  // join can't be called from an io thread.  stop _can_ be
  // called from an io thread
  static void stop();

  // block until all io threads have terminated.  If stop
  // has not already been called, it will be called by join.
  // note that join will _hang_ if called from an io thread
  // (other than the implicit io thread that called
  // start_this_thread if that was called).
  static void join();

  // base class associated with the action taken when there
  // is io available on a watched file descriptor
  class handler
  {
  public:
    virtual void io_avail(const struct epoll_event &event) = 0;
  };

  // This function will call the system epoll_ctl, using the given
  // events, hnd, and this io_dispatch's ep_fd_.  Whenever io is
  // available on the given fd (according to 'events'), the given
  // hnd->io_avail will be called.  For that call the 'io_d' parameter
  // will be this io_dispatch, and the 'event' parameter will be the
  // one filled out by epoll_wait
  static void epoll_ctl(int op, int fd, uint32_t events, handler *hnd)
  {
    struct epoll_event evt;
    evt.events = events;
    evt.data.ptr = hnd;
    if (::epoll_ctl(io_d.ep_fd_, op, fd, &evt) < 0)
      do_error("epoll_ctl(ep_fd_, " << op_string(op) << ", " << fd << ", &evt)");
  }

  // pause all io threads (other than the one calling this function
  // if it happens to be called from an io thread) and execute 'f'
  // while they are paused. Once 'f' returns resume all io threads.
  // Note that this is not guaranteed to run.  If a while_paused is
  // already in progress when this is called it cannot run another
  // until that completes.  It returns true if it was able to run
  // the while_paused function, false if not
  static bool while_paused(const std::function<void(void)> &f)
  {
    // if this is called from an io thread, then there
    // is one less io thread we have to pause
    bool is_io_thread = io_d.on_io_thread();

    std::unique_lock<std::mutex> lock(io_d.pause_outer_mutex_, std::try_to_lock);
    if (!lock)
    {
#if defined(ANON_DEBUG_PAUSED)
      anon_log("while_paused failed to lock pause_outer_mutex_");
#endif
      return false;
    }

    anon::unique_lock<std::mutex> com_lock(io_d.pause_com_mutex_);
    io_d.num_paused_threads_ = is_io_thread ? 1 : 0;

    // write the pause commands for all of the (other) io threads
    for (auto i = io_d.num_paused_threads_; i < io_d.num_threads_; i++)
    {
      char cmd = k_pause;
      if (write(io_d.send_ctl_fd_, &cmd, 1) != 1)
        do_error("write(io_d.send_ctl_fd_, &cmd, 1)");
    }

    while (io_d.num_paused_threads_ != io_d.num_threads_)
      io_d.pause_cond_.wait(com_lock);

    f();

    io_d.num_paused_threads_ = 0;
    io_d.resume_cond_.notify_all();

    return true;
  }

  // similar to the above while_paused, except called with a
  // a function which returns another function object.  That
  // returned function will be called once all io threads have
  // returned at least once from epoll_wait, processed whatever
  // events that returned, and are all just about to call epoll_wait
  // again.  This can be used to "flush" io events from the system.
  static bool while_paused2(const std::function<std::function<void(void)>(void)> &f)
  {
    // currently only supported when called from an io thread
    bool is_io_thread = io_d.on_io_thread();
    if (!is_io_thread)
      anon_throw(std::runtime_error, "invalid call to while_paused2 from a non-io thread");

    std::unique_lock<std::mutex> lock(io_d.pause_outer_mutex_, std::try_to_lock);
    if (!lock)
    {
#if defined(ANON_DEBUG_PAUSED)
      anon_log("while_paused2 failed to lock pause_outer_mutex_");
#endif
      return false;
    }

    anon::unique_lock<std::mutex> com_lock(io_d.pause_com_mutex_);
#if defined(ANON_DEBUG_PAUSED)
    anon_log("while_paused2 locked pause_outer_mutex_ and pause_com_mutex_");
#endif

    io_d.num_paused_threads_ = 1;

    // write the pause commands for all of the (other) io threads
    for (auto i = io_d.num_paused_threads_; i < io_d.num_threads_; i++)
    {
      char cmd = k_pause;
      if (write(io_d.send_ctl_fd_, &cmd, 1) != 1)
        do_error("write(io_d.send_ctl_fd_, &cmd, 1)");
    }

    while (io_d.num_paused_threads_ != io_d.num_threads_)
      io_d.pause_cond_.wait(com_lock);

#if defined(ANON_DEBUG_PAUSED)
    anon_log("all io threads paused");
#endif

    // call the "while paused" function and store
    // its return value in our list of functions
    // we will call once all io threads get back
    // to where they will call epoll_wait again
    try
    {
      io_d.add_at_rest_fn(f());
    }
    catch (...)
    {
      anon_log_error("io_d.add_at_rest_fn(f())io_d.add_at_rest_fn(f()) threw exception");
    }

    io_d.thread_countdown_ += io_d.num_threads_;
    set_this_thread_countdown();
    io_d.num_paused_threads_ = 0;
    io_d.num_pause_done_threads_ = 1;
    io_d.resume_cond_.notify_all();

    // we need to wait here until all other io threads
    // have seen that num_paused_threads_ is zero and
    // have move on from waiting for it to be zero.
    while (io_d.num_pause_done_threads_ != io_d.num_threads_)
      io_d.resume2_cond_.wait(com_lock);

    return true;
  }

  // execute the given function once on each
  // of the io threads
  template <typename Fn>
  static bool on_each(Fn f)
  {
    // if this is called from an io thread, then there
    // is one less io thread we have to cause to execute f
    bool is_io_thread = io_d.on_io_thread();

    std::unique_lock<std::mutex> lock(io_d.pause_outer_mutex_, std::try_to_lock);
    if (!lock)
    {
#if defined(ANON_DEBUG_PAUSED)
      anon_log("on_each failed to lock pause_outer_mutex_");
#endif
      return false;
    }

    io_d.num_paused_threads_ = is_io_thread ? 1 : 0;

    // if there is only one io thread, and
    // we are called from that thread, then
    // we don't want to write a k_on_each command
    if (io_d.num_paused_threads_ < io_d.num_threads_)
    {
      auto tc = new virt_caller<Fn>(f);
      char buf[1 + sizeof(tc)];
      buf[0] = k_on_each;
      memcpy(&buf[1], &tc, sizeof(tc));
      if (write(io_d.send_ctl_fd_, &buf[0], sizeof(buf)) != sizeof(buf))
        do_error("write(io_d.send_ctl_fd_, &buf[0], sizeof(buf))");
    }

    while (io_d.num_paused_threads_ != io_d.num_threads_)
      io_d.pause_cond_.wait(lock);

    if (is_io_thread)
      f();

    io_d.num_paused_threads_ = 0;
    io_d.resume_cond_.notify_all();

    return true;
  }

  // if you have passed numSigs > 0 to io_dispatch::start
  // then this routine can be called to "retrieve" the
  // signal numbers.  It is assumed that each client that
  // is interested in using its own signal number would call
  // this method once to get that signal number and then use
  // it as appropriate.
  static int getSigNum()
  {
    int sig = io_d.curSig_++;
    if (sig >= io_d.endSig_)
      do_error("no more available masked signals");
    return sig;
  }

  // execute the given function on (exactly) one
  // of the io threads
  template <typename Fn>
  static void on_one(const Fn &f)
  {
    // if this is an io thread we can just call it here
    // note that this means that if you happen to be calling
    // from a fiber context then f will run in that fiber
    // context.
    if (io_d.on_io_thread())
      f();
    else
    {
      char buf[k_oo_command_buf_size];
      on_one_command(f, buf);
      if (write(io_d.send_ctl_fd_, &buf[0], sizeof(buf)) != sizeof(buf))
        do_error("write(io_d.send_ctl_fd_, &buf[0], sizeof(buf))");
    }
  }

  enum
  {
    k_oo_command_buf_size = 1 + sizeof(void *)
  };

  template <typename Fn>
  static void on_one_command(const Fn &f, char (&buf)[1 + sizeof(void *)])
  {
    auto tc = new virt_caller<Fn>(f);
    buf[0] = k_on_one;
    memcpy(&buf[1], &tc, sizeof(tc));
  }

  struct scheduled_task
  {
    struct timespec when_;
    int id_;

    scheduled_task()
    {
      memset(&when_, 0, sizeof(when_));
      id_ = 0;
    }

    scheduled_task(const struct timespec &when, int id)
        : when_(when),
          id_(id)
    {
    }
  };

  template <typename Fn>
  static scheduled_task schedule_task(Fn f, const struct timespec &when)
  {
    return io_d.schedule_task_(new virt_caller<Fn>(f), when);
  }

  static bool remove_task(const scheduled_task &task);

  static int new_command_pipe();

#if defined(ANON_RUNTIME_CHECKS)
  static bool is_io_dispatch_thread()
  {
    return io_d.on_io_thread();
  }
#endif

private:
  static std::string op_string(int op)
  {
    switch (op)
    {
    case EPOLL_CTL_ADD:
      return "EPOLL_CTL_ADD";
    case EPOLL_CTL_MOD:
      return "EPOLL_CTL_MOD";
    case EPOLL_CTL_DEL:
      return "EPOLL_CTL_DEL";
    default:
      return std::string("unknown (") + std::to_string(op) + ")";
    }
  }

  bool on_io_thread()
  {
    bool is_io_thread = false;
    int tid = syscall(SYS_gettid);
    for (int i = 0; i < num_threads_; i++)
      if (tid == io_thread_ids_[i])
      {
        is_io_thread = true;
        break;
      }
    return is_io_thread;
  }

  struct virt_caller_
  {
  public:
    virt_caller_() : id_(next_id_++) {}
    virtual ~virt_caller_() {}
    virtual void exec() = 0;
    int id_;

  private:
    static std::atomic<int> next_id_;
  };

  template <typename Fn>
  struct virt_caller : public virt_caller_
  {
  public:
    virt_caller(const Fn &f)
        : f_(f)
    {
    }

    virtual void exec()
    {
      f_();
    }

  private:
    Fn f_;
  };

  scheduled_task schedule_task_(virt_caller_ *task, const struct timespec &when);

  enum
  {
    k_wake = 0,
    k_pause = 1,
    k_on_each = 2,
    k_on_one = 3
  };

  io_dispatch(const io_dispatch &);
  io_dispatch(io_dispatch &&);

  friend class io_ctl_handler;
  friend class io_timer_handler;

  void epoll_loop();
  void wake_next_thread();
  void add_at_rest_fn(const std::function<void(void)> &fn);
  static void set_this_thread_countdown();

  std::condition_variable thread_countdown_cond_;
  std::atomic_int thread_countdown_;
  std::list<std::function<void(void)>> at_rest_functions_;

  bool running_;
  int ep_fd_;
  int send_ctl_fd_;
  std::vector<io_ctl_handler *> io_ctl_handlers_;
  int timer_fd_;
  std::vector<std::thread> io_threads_;
  std::vector<int> io_thread_ids_;
  std::atomic_int thread_init_index_;
  int num_threads_;

  std::mutex pause_outer_mutex_;
  std::mutex pause_com_mutex_;
  std::condition_variable pause_cond_;
  std::condition_variable resume_cond_;
  std::condition_variable resume2_cond_;
  int num_paused_threads_;
  int num_pause_done_threads_;

  std::mutex task_mutex_;
  std::multimap<struct timespec, std::unique_ptr<virt_caller_>> task_map_;
  std::atomic_int curSig_;
  int endSig_;

  static io_dispatch io_d;
};

inline std::string event_bits_to_string(uint32_t event_bits)
{
  std::string eventstr;
  if (event_bits & EPOLLIN)
    eventstr += "EPOLLIN ";
  if (event_bits & EPOLLPRI)
    eventstr += "EPOLLPRI ";
  if (event_bits & EPOLLOUT)
    eventstr += "EPOLLOUT ";
  if (event_bits & EPOLLRDNORM)
    eventstr += "EPOLLRDNORM ";
  if (event_bits & EPOLLRDBAND)
    eventstr += "EPOLLRDBAND ";
  if (event_bits & EPOLLWRNORM)
    eventstr += "EPOLLWRNORM ";
  if (event_bits & EPOLLWRBAND)
    eventstr += "EPOLLWRBAND ";
  if (event_bits & EPOLLMSG)
    eventstr += "EPOLLMSG ";
  if (event_bits & EPOLLERR)
    eventstr += "EPOLLERR ";
  if (event_bits & EPOLLHUP)
    eventstr += "EPOLLHUP ";
  if (event_bits & EPOLLRDHUP)
    eventstr += "EPOLLRDHUP ";
  if (event_bits & EPOLLONESHOT)
    eventstr += "EPOLLONESHOT ";
  if (event_bits & EPOLLET)
    eventstr += "EPOLLET ";
  return eventstr;
}

template <typename T>
T &operator<<(T &str, const io_dispatch::scheduled_task &task)
{
  return str << "{" << task.when_ << ", " << task.id_ << "}";
}

inline bool operator<(const io_dispatch::scheduled_task &t1, const io_dispatch::scheduled_task &t2)
{
  if (t1.when_ < t2.when_)
    return true;
  if (t2.when_ < t1.when_)
    return false;
  return t1.id_ < t2.id_;
}
