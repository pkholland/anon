
#include "io_dispatch.h"
#include "log.h"
#include <sys/epoll.h>
#include <system_error>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/timerfd.h>

class io_ctl_handler : public io_dispatch::handler
{
public:
  io_ctl_handler(int fd)
    : fd_(fd)
  {}
  
  ~io_ctl_handler()
  {
    close(fd_);
  }
  
  virtual void io_avail(io_dispatch& io_d, const struct epoll_event& evt)
  {
    if (evt.events & EPOLLIN) {
      char cmd;
      if (read(fd_, &cmd, 1) != 1)
        do_error("read(fd_, &cmd, 1)");
        
      if (cmd == io_dispatch::k_wake) {
      
        io_d.epoll_ctl(EPOLL_CTL_MOD, fd_, EPOLLIN | EPOLLONESHOT, this);
        io_d.wake_next_thread();
        
      } else if (cmd == io_dispatch::k_pause) {
      
        std::unique_lock<std::mutex> lock(io_d.pause_mutex_);
        io_d.epoll_ctl(EPOLL_CTL_MOD, fd_, EPOLLIN | EPOLLONESHOT, this);
        
        // if this is the last io thread to have paused
        // then signal whatever thread is waiting in
        // io_dispatch::while_paused, otherwise get the
        // next io thread to pause
        if (++io_d.num_paused_threads_ == io_d.num_threads_)
          io_d.pause_cond_.notify_one();
        else {
          if (write(io_d.send_ctl_fd_,&cmd,1) != 1)
            do_error("write(io_d.send_ctl_fd_,&cmd,1)");
        }
        
        // wait until the thread that called io_dispatch::while_paused
        // to run its function and signal that everyone can
        // continue
        while (io_d.num_paused_threads_ != 0)
          io_d.resume_cond_.wait(lock);
          
      } else if (cmd == io_dispatch::k_on_each) {
      
        std::unique_lock<std::mutex> lock(io_d.pause_mutex_);
        
        io_dispatch::thread_caller_ *tc;
        if (read(fd_, &tc, sizeof(tc)) != sizeof(tc))
          do_error("read(fd_, &tc, sizeof(tc))");
        io_d.epoll_ctl(EPOLL_CTL_MOD, fd_, EPOLLIN | EPOLLONESHOT, this);
        
        tc->exec();
        
        if (++io_d.num_paused_threads_ == io_d.num_threads_) {
          delete tc;
          io_d.pause_cond_.notify_one();
        } else {
          char buf[1+sizeof(tc)];
          buf[0] = io_dispatch::k_on_each;
          memcpy(&buf[1],&tc,sizeof(tc));
          if (write(io_d.send_ctl_fd_,&buf[0],sizeof(buf)) != sizeof(buf))
            do_error("write(io_d.send_ctl_fd_,&buf[0],sizeof(buf))");
        }
        
        // wait until the thread that called io_dispatch::on_each
        // to run its function and signal that everyone can
        // continue
        while (io_d.num_paused_threads_ != 0)
          io_d.resume_cond_.wait(lock);
          
      } else if (cmd == io_dispatch::k_on_one) {
      
        io_dispatch::thread_caller_ *tc;
        if (read(fd_, &tc, sizeof(tc)) != sizeof(tc))
          do_error("read(ctl_fd_, &tc, sizeof(tc))");
        io_d.epoll_ctl(EPOLL_CTL_MOD, fd_, EPOLLIN | EPOLLONESHOT, this);
        tc->exec();
        delete tc;
        
      } else
        anon_log_error("unknown command (" << (int)cmd << ") written to control pipe - will be ignored" );
        
    } else
      anon_log_error("io_ctl_handler::io_avail called with event that does not have EPOLLIN set - control pipe now broken!");
  }
  
private:
  int fd_;
};

//static io_ctl_handler ctl_handler;

///////////////////////////////////////////////////////////////////////////

class io_timer_handler : public io_dispatch::handler
{
  virtual void io_avail(io_dispatch& io_d, const struct epoll_event& evt)
  {
    if (evt.events & EPOLLIN) {
      uint64_t num_expirations;
      if (read(io_d.timer_fd_, &num_expirations, sizeof(num_expirations)) != sizeof(num_expirations)) {
        // note that we can get EAGAIN in certain cases where the kernel signals multiple
        // threads that are calling epoll_wait.  In that case only one will get the data
        // and the others will get EAGAIN.  So if this is one of those other threads we
        // ignore it.
        if (errno == EAGAIN)
          return;
        do_error("read(io_d.timer_fd_, &num_expirations, sizeof(num_expirations))");
      }
      
      struct timespec cur_time;
      if (clock_gettime(CLOCK_MONOTONIC, &cur_time) != 0)
        do_error("clock_gettime(CLOCK_MONOTONIC, &cur_time)");
        
      std::vector<io_dispatch::scheduled_task*> ready_tasks;
      {
        std::unique_lock<std::mutex> lock(io_d.task_mutex_);
        std::multimap<struct timespec,io_dispatch::scheduled_task*>::iterator beg;
        while (((beg=io_d.task_map_.begin()) != io_d.task_map_.end()) && (beg->first <= cur_time)) {
          ready_tasks.push_back(beg->second);
          io_d.task_map_.erase(beg);
        }
        if ((beg=io_d.task_map_.begin()) != io_d.task_map_.end()) {
          struct itimerspec t_spec = { 0 };
          t_spec.it_value = beg->first;
          if (timerfd_settime(io_d.timer_fd_, TFD_TIMER_ABSTIME, &t_spec, 0) != 0)
            do_error("timerfd_settime(id_d.timer_fd_, TFD_TIMER_ABSTIME, &t_spec, 0)");
        }
      }

      for (auto task : ready_tasks)
        task->exec();
    }
  }
};

static io_timer_handler timer_handler;

///////////////////////////////////////////////////////////////////////////

io_dispatch::io_dispatch(int num_threads, bool use_this_thread)
  : running_(true),
    num_threads_(num_threads)
{
  ep_fd_ = epoll_create1(EPOLL_CLOEXEC);
  if (ep_fd_ < 0)
    do_error("epoll_create1(EPOLL_CLOEXEC)");
    
  anon_log("using fd " << ep_fd_ << " for epoll");
  
  send_ctl_fd_ = new_command_pipe();
  
  // the file descriptor we use for the timer
  timer_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (timer_fd_ == -1)
    do_error("timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC)");
  epoll_ctl(EPOLL_CTL_ADD, timer_fd_, EPOLLIN, &timer_handler);
  anon_log("using fd " << timer_fd_ << " for timer");
  
  io_thread_ids_.resize(num_threads,0);
  thread_init_index_.store(0);
  if (use_this_thread)
    --num_threads;
  for (int i = 0; i < num_threads; i++)
    io_threads_.push_back(std::thread(std::bind(&io_dispatch::epoll_loop,this)));
}

io_dispatch::~io_dispatch()
{
  stop();
  for (auto thread = io_threads_.begin(); thread != io_threads_.end(); ++thread)
    thread->join();
  for (auto rcvh = io_ctl_handlers_.begin(); rcvh != io_ctl_handlers_.end(); ++rcvh)
    delete *rcvh;
  close(send_ctl_fd_);
  close(ep_fd_);
  close(timer_fd_);
}

void io_dispatch::stop()
{
  if (running_) {
    running_ = false;
    wake_next_thread();
  }
}

int io_dispatch::new_command_pipe()
{
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0)
    do_error("socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv)");
    
  // reduce the size of the pipe buffers to keep callers from
  // queueing up too many requests in advance of us being able
  // to dispatch them
  const int bufSize = 4096;
  socklen_t optSize = sizeof(bufSize);
  if (setsockopt(sv[0], SOL_SOCKET, SO_RCVBUF, &bufSize, optSize) != 0)
    do_error("setsockopt(sv[0], SOL_SOCKET, SO_RCVBUF, &bufSize, optSize)");
  if (setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &bufSize, optSize) != 0)
    do_error("setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &bufSize, optSize)");
  if (setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &bufSize, optSize) != 0)
    do_error("setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &bufSize, optSize)");
  if (setsockopt(sv[1], SOL_SOCKET, SO_SNDBUF, &bufSize, optSize) != 0)
    do_error("setsockopt(sv[1], SOL_SOCKET, SO_SNDBUF, &bufSize, optSize)");
    
  anon_log("using fds " << sv[0] << " (send), and " <<  sv[1] << " (receive) for io threads control pipe");
  
  auto hnd = new io_ctl_handler(sv[1]);
  io_ctl_handlers_.push_back(hnd);
  
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

void io_dispatch::epoll_loop()
{
  anon_log("starting io_dispatch::epoll_loop");
  
  // record this thread id
  auto index = thread_init_index_.fetch_add(1,std::memory_order_relaxed);
  if (index >= num_threads_) {
    anon_log_error("too many calls to io_dispatch::epoll_loop");
    throw std::runtime_error("io_dispatch::epoll_loop");
  }
  io_thread_ids_[index] = syscall(SYS_gettid);

  while (running_)  {
  
    struct epoll_event event;
    int ret;
    if ((ret = epoll_wait(ep_fd_, &event, 1, -1)) > 0) {
    
      ((handler*)event.data.ptr)->io_avail(*this, event);
    
    } else if ((ret != 0) && (errno != EINTR)) {
    
      // if we have a debugger attached, then every time the debugger hits a break point
      // it sends signals to all the threads, and we end up coming out of epoll_wait with
      // errno set to EINTR.  That's not worth printing...
      anon_log_error("epoll_wait returned error with errno: " << errno_string());
      
    }
    
  }
  
  anon_log("exiting io_dispatch::epoll_loop");
}

void io_dispatch::schedule_task(scheduled_task* task, const timespec& when)
{
  task->when_ = when;
  std::unique_lock<std::mutex>  lock(task_mutex_);
  task_map_.insert(std::make_pair(task->when_,task));
  if (task_map_.begin()->first == task->when_) {
    struct itimerspec t_spec = { 0 };
    t_spec.it_value = task->when_;
    if (timerfd_settime(timer_fd_, TFD_TIMER_ABSTIME, &t_spec, 0) != 0)
      do_error("timerfd_settime(timer_fd_, TFD_TIMER_ABSTIME, &t_spec, 0)");
  }
}

bool io_dispatch::remove_task(scheduled_task* task)
{
  std::unique_lock<std::mutex>  lock(task_mutex_);
  auto it = task_map_.find(task->when_);
  while (it != task_map_.end() && it->second != task && it->first == task->when_)
    ++it;
  if (it != task_map_.end() && it->second == task) {
    task_map_.erase(it);
    return true;
  }
  return false;
}

