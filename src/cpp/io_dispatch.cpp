/*
 Copyright (c) 2019 Anon authors, see AUTHORS file.
 
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

#include "io_dispatch.h"
#include <sys/epoll.h>
#include <system_error>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <openssl/err.h>
#include <sys/signalfd.h>
#include <cxxabi.h>


namespace
{

struct iod_params
{

  iod_params()
      : countdown_(0)
  {
  }

  int countdown_;
};

thread_local iod_params tls_iod_params;

} // namespace

class io_ctl_handler : public io_dispatch::handler
{
public:
  io_ctl_handler(int fd)
      : fd_(fd)
  {
  }

  ~io_ctl_handler()
  {
    close(fd_);
  }

  virtual void io_avail(const struct epoll_event &evt)
  {
    io_dispatch &io_d = io_dispatch::io_d;

    if (evt.events & EPOLLIN)
    {
      char cmd;
      if (read(fd_, &cmd, 1) != 1)
        do_error("read(fd_, &cmd, 1)");

      if (cmd == io_dispatch::k_wake)
      {
        io_dispatch::epoll_ctl(EPOLL_CTL_MOD, fd_, EPOLLIN | EPOLLONESHOT, this);
        io_d.wake_next_thread();
      }
      else if (cmd == io_dispatch::k_pause)
      {
        anon::unique_lock<std::mutex> lock(io_d.pause_com_mutex_);
        io_dispatch::epoll_ctl(EPOLL_CTL_MOD, fd_, EPOLLIN | EPOLLONESHOT, this);

        // if this is the last io thread to have paused
        // then signal whatever thread is waiting in
        // io_dispatch::while_paused
        if (++io_d.num_paused_threads_ == io_d.num_threads_)
        {
#if defined(ANON_DEBUG_PAUSED)
          anon_log(" notifying all paused");
#endif
          io_d.pause_cond_.notify_one();
        }

        // wait until the thread that called io_dispatch::while_paused
        // to run its function and signal that everyone can
        // continue
        while (io_d.num_paused_threads_ != 0)
          io_d.resume_cond_.wait(lock);

        // tell while_paused2 that everyone has made it past
        // the wait immediately above
        if (++io_d.num_pause_done_threads_ == io_d.num_threads_)
        {
#if defined(ANON_DEBUG_PAUSED)
          anon_log("all io threads have resumed, notifying while_paused2");
#endif
          io_d.resume2_cond_.notify_all();
        }

        // signal this thread to execute the "countdown"
        // when it is ready to call epoll_wait again
        ++tls_iod_params.countdown_;
      }
      else if (cmd == io_dispatch::k_on_each)
      {
        anon::unique_lock<std::mutex> lock(io_d.pause_outer_mutex_);

        io_dispatch::virt_caller_ *tc;
        if (read(fd_, &tc, sizeof(tc)) != sizeof(tc))
          do_error("read(fd_, &tc, sizeof(tc))");
        io_d.epoll_ctl(EPOLL_CTL_MOD, fd_, EPOLLIN | EPOLLONESHOT, this);

        tc->exec();

        if (++io_d.num_paused_threads_ == io_d.num_threads_)
        {
          delete tc;
          io_d.pause_cond_.notify_one();
        }
        else
        {
          char buf[1 + sizeof(tc)];
          buf[0] = io_dispatch::k_on_each;
          memcpy(&buf[1], &tc, sizeof(tc));
          if (write(io_d.send_ctl_fd_, &buf[0], sizeof(buf)) != sizeof(buf))
            do_error("write(io_d.send_ctl_fd_,&buf[0],sizeof(buf))");
        }

        // wait until the thread that called io_dispatch::on_each
        // to run its function and signal that everyone can
        // continue
        while (io_d.num_paused_threads_ != 0)
          io_d.resume_cond_.wait(lock);
      }
      else if (cmd == io_dispatch::k_on_one)
      {
        io_dispatch::virt_caller_ *tc;
        if (read(fd_, &tc, sizeof(tc)) != sizeof(tc))
          do_error("read(ctl_fd_, &tc, sizeof(tc))");
        io_d.epoll_ctl(EPOLL_CTL_MOD, fd_, EPOLLIN | EPOLLONESHOT, this);
        tc->exec();
        delete tc;
      }
      else
        anon_log_error("unknown command (" << (int)cmd << ") written to control pipe - will be ignored");
    }
    else
      anon_log_error("io_ctl_handler::io_avail called with event that does not have EPOLLIN set - control pipe now broken!");
  }

private:
  int fd_;
};

//static io_ctl_handler ctl_handler;

///////////////////////////////////////////////////////////////////////////

class io_timer_handler : public io_dispatch::handler
{
  virtual void io_avail(const struct epoll_event &evt)
  {
    io_dispatch &io_d = io_dispatch::io_d;

    if (evt.events & EPOLLIN)
    {
      uint64_t num_expirations;
      if (read(io_d.timer_fd_, &num_expirations, sizeof(num_expirations)) != sizeof(num_expirations))
      {
        // note that we can get EAGAIN in certain cases where the kernel signals multiple
        // threads that are calling epoll_wait.  In that case only one will get the data
        // and the others will get EAGAIN.  So if this is one of those other threads we
        // ignore it.
        if (errno == EAGAIN)
          return;
        do_error("read(io_d.timer_fd_, &num_expirations, sizeof(num_expirations))");
      }

#if defined(ANON_DEBUG_TIMERS)
      anon_log("read timer event");
#endif

      struct timespec now = cur_time();

      std::vector<std::unique_ptr<io_dispatch::virt_caller_>> ready_tasks;
      {
        anon::unique_lock<std::mutex> lock(io_d.task_mutex_);
        decltype(io_d.task_map_.begin()) beg;
        while (((beg = io_d.task_map_.begin()) != io_d.task_map_.end()) && (beg->first <= now))
        {
#if defined(ANON_DEBUG_TIMERS)
          anon_log("removed timer callback from list");
#endif
          ready_tasks.push_back(std::move(beg->second));
          io_d.task_map_.erase(beg);
        }
        if (beg != io_d.task_map_.end())
        {
#if defined(ANON_DEBUG_TIMERS)
          anon_log("timer list non-empty, rearming timer to " << beg->first - cur_time() << " seconds in future");
#endif
          struct itimerspec t_spec = {0};
          t_spec.it_value = beg->first;
          if (timerfd_settime(io_d.timer_fd_, TFD_TIMER_ABSTIME, &t_spec, 0) != 0)
            do_error("timerfd_settime(id_d.timer_fd_, TFD_TIMER_ABSTIME, &t_spec, 0)");
        }
      }

      for (auto &it : ready_tasks)
        it->exec();
    }
  }
};

static io_timer_handler timer_handler;

///////////////////////////////////////////////////////////////////////////

// the singleton
io_dispatch io_dispatch::io_d;

std::atomic<int> io_dispatch::virt_caller_::next_id_;

io_dispatch::io_dispatch()
    : running_(false),
      thread_countdown_(0)
{
}

io_dispatch::~io_dispatch()
{
}

void io_dispatch::start(int num_threads, bool use_this_thread, int numSigFds, int firstSigFdSig)
{
#if defined(ANON_RUNTIME_CHECKS)
  if (io_d.running_)
    anon_throw(std::runtime_error, "io_dispatch::start already called");
#endif

  io_d.curSig_ = firstSigFdSig;
  io_d.endSig_ = firstSigFdSig + numSigFds;
  sigset_t sigs;
  sigemptyset(&sigs);
  sigaddset(&sigs, SIGPIPE); // we never want this one
  for (auto sfd = 0; sfd < numSigFds; sfd++)
    sigaddset(&sigs, firstSigFdSig + sfd);
  pthread_sigmask(SIG_BLOCK, &sigs, NULL);

  io_d.running_ = true;
  io_d.num_threads_ = num_threads;

  io_d.ep_fd_ = epoll_create1(EPOLL_CLOEXEC);
  if (io_d.ep_fd_ < 0)
    do_error("epoll_create1(EPOLL_CLOEXEC)");

  anon_log("using fd " << io_d.ep_fd_ << " for epoll");

  io_d.send_ctl_fd_ = new_command_pipe();

  // the file descriptor we use for the timer
  io_d.timer_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (io_d.timer_fd_ == -1)
    do_error("timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC)");
  epoll_ctl(EPOLL_CTL_ADD, io_d.timer_fd_, EPOLLIN, &timer_handler);
  anon_log("using fd " << io_d.timer_fd_ << " for timer");

  io_d.io_thread_ids_.resize(num_threads, 0);
  io_d.thread_init_index_.store(0);
  if (use_this_thread)
    --num_threads;
  for (int i = 0; i < num_threads; i++)
    io_d.io_threads_.push_back(std::thread(std::bind(&io_dispatch::epoll_loop, &io_d)));
}

void io_dispatch::start_this_thread()
{
  io_d.epoll_loop();
}

void io_dispatch::stop()
{
  if (io_d.running_)
  {
    io_d.running_ = false;
    io_d.wake_next_thread();
  }
}

void io_dispatch::join()
{
  stop();
  for (auto thread = io_d.io_threads_.begin(); thread != io_d.io_threads_.end(); ++thread)
    thread->join();
  for (auto rcvh = io_d.io_ctl_handlers_.begin(); rcvh != io_d.io_ctl_handlers_.end(); ++rcvh)
    delete *rcvh;
  close(io_d.send_ctl_fd_);
  close(io_d.ep_fd_);
  close(io_d.timer_fd_);
}

int io_dispatch::new_command_pipe()
{
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0)
    do_error("socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv)");

  // reduce the size of the pipe buffers to keep callers from
  // queueing up too many requests in advance of us being able
  // to dispatch them
  const int bufSize = 32768;
  socklen_t optSize = sizeof(bufSize);
  if (setsockopt(sv[0], SOL_SOCKET, SO_RCVBUF, &bufSize, optSize) != 0)
    do_error("setsockopt(sv[0], SOL_SOCKET, SO_RCVBUF, &bufSize, optSize)");
  if (setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &bufSize, optSize) != 0)
    do_error("setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &bufSize, optSize)");
  if (setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &bufSize, optSize) != 0)
    do_error("setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &bufSize, optSize)");
  if (setsockopt(sv[1], SOL_SOCKET, SO_SNDBUF, &bufSize, optSize) != 0)
    do_error("setsockopt(sv[1], SOL_SOCKET, SO_SNDBUF, &bufSize, optSize)");

  anon_log("using fds " << sv[0] << " (send), and " << sv[1] << " (receive) for io threads control pipe");

  auto hnd = new io_ctl_handler(sv[1]);
  io_d.io_ctl_handlers_.push_back(hnd);

  // hook up the handler for the control pipe
  // we use (level triggered) one-shot to give ourselves
  // control in getting the control pipe reads to happen
  // serially, and not just have all threads simultaneously
  // processing control commands (EPOLLIN on its own), or
  // force us to conintue to call read until we get EAGAIN
  // in order to re-arm the event (EPOLLET)
  epoll_ctl(EPOLL_CTL_ADD, sv[1], EPOLLIN | EPOLLONESHOT, hnd);

  return sv[0];
}

void io_dispatch::wake_next_thread()
{
  char cmd = k_wake;
  if (write(send_ctl_fd_, &cmd, 1) != 1)
    anon_log_error("write of k_wake command failed with errno: " << errno_string());
}

int guess_fd(io_dispatch::handler *ioh);

void io_dispatch::add_at_rest_fn(const std::function<void(void)> &fn)
{
  at_rest_functions_.push_front(fn);
}

void io_dispatch::set_this_thread_countdown()
{
  ++tls_iod_params.countdown_;
};

void io_dispatch::epoll_loop()
{
  anon_log("starting io_dispatch::epoll_loop");

  // for any implementation of libstdc++ where there is a difference
  // in speed between __cxa_get_globals and __cxa_get_globals_fast,
  // we make sure that any thread that runs fibers has made at least
  // one call to __cxa_get_globals prior to getting started with
  // fiber switching - which can then call __cxa_get_globals_fast.
  (void)__cxxabiv1::__cxa_get_globals();

  // record this thread id
  auto index = thread_init_index_.fetch_add(1, std::memory_order_relaxed);
  if (index >= num_threads_)
    anon_throw(std::runtime_error, "too many calls to io_dispatch::epoll_loop");
  io_thread_ids_[index] = syscall(SYS_gettid);

  while (running_)
  {
    // deal with any "stage2" at_rest
    // while_paused operations that may have
    // been registered
    auto &cd = tls_iod_params.countdown_;
    if (cd)
    {
      auto ncd = thread_countdown_ -= cd;
      cd = 0;
      if (ncd == 0)
      {
        std::unique_lock<std::mutex> lock(pause_com_mutex_);
// we execute all at_rest functions with
// pause_mutex_ locked.  That keeps
// any other io threads blocked in the while
// statement below, which ensures that only
// this thread is running while these functions
// execute
#if defined(ANON_DEBUG_PAUSED)
        auto num_at_rest = at_rest_functions_.size();
        anon_log("executing " << num_at_rest << " at rest function" << (num_at_rest == 1 ? "" : "s"));
#endif
        for (auto &arf : at_rest_functions_)
          arf();
        at_rest_functions_.clear();
        thread_countdown_cond_.notify_all();
      }
    }

    struct epoll_event event[1];
    int ret;
    if ((ret = epoll_wait(ep_fd_, &event[0], sizeof(event) / sizeof(event[0]), -1)) > 0)
    {

      for (int i = 0; i < ret; i++)
        ((handler *)event[i].data.ptr)->io_avail(event[i]);
    }
    else if ((ret != 0) && (errno != EINTR))
    {

      // if we have a debugger attached, then every time the debugger hits a break point
      // it sends signals to all the threads, and we end up coming out of epoll_wait with
      // errno set to EINTR.  That's not worth printing...
      anon_log_error("epoll_wait returned error with errno: " << errno_string());
    }
  }

  anon_log("exiting io_dispatch::epoll_loop");

  // clean up some potential openssl memory
  // in older versions of openssl
  #if OPENSSL_VERSION_NUMBER < 0x10100000
  ERR_remove_state(0);
  #endif
}

io_dispatch::scheduled_task io_dispatch::schedule_task_(virt_caller_ *task, const timespec &when)
{
  anon::unique_lock<std::mutex> lock(io_d.task_mutex_);
  io_d.task_map_.insert(std::make_pair(when, std::unique_ptr<virt_caller_>(task)));
  if (io_d.task_map_.begin()->first == when)
  {
#if defined(ANON_DEBUG_TIMERS)
    anon_log("schedule_task_ setting timer_fd to " << when - cur_time() << " seconds in future");
#endif
    struct itimerspec t_spec = {0};
    t_spec.it_value = when;
    if (timerfd_settime(io_d.timer_fd_, TFD_TIMER_ABSTIME, &t_spec, 0) != 0)
      do_error("timerfd_settime(io_d.timer_fd_, TFD_TIMER_ABSTIME, &t_spec, 0)");
  }
  return scheduled_task(when, task->id_);
}

bool io_dispatch::remove_task(const scheduled_task &task)
{
  anon::unique_lock<std::mutex> lock(io_d.task_mutex_);
  auto it = io_d.task_map_.find(task.when_);
  while (it != io_d.task_map_.end() && it->second->id_ != task.id_ && it->first == task.when_)
    ++it;
  if (it != io_d.task_map_.end() && it->second->id_ == task.id_)
  {
    io_d.task_map_.erase(it);
    return true;
  }
  return false;
}
