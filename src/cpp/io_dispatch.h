#pragma once

#include "log.h"
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <map>
#include <atomic>
#include <sys/epoll.h>
#include <string.h>

inline bool operator<(const struct timespec& spec1, const struct timespec& spec2)
{
  if (spec1.tv_sec != spec2.tv_sec)
    return spec1.tv_sec < spec2.tv_sec;
  return spec1.tv_nsec < spec2.tv_nsec;
}

inline bool operator<=(const struct timespec& spec1, const struct timespec& spec2)
{
  return !operator<(spec2, spec1);
}

inline struct timespec operator+(const struct timespec& spec1, const struct timespec& spec2)
{
  struct timespec spec;
  spec.tv_sec = spec1.tv_sec + spec2.tv_sec;
  spec.tv_nsec = spec1.tv_nsec + spec2.tv_nsec;
  if (spec.tv_nsec > 1000000000) {
    ++spec.tv_sec;
    spec.tv_nsec -= 1000000000;
  }
  return spec;
}

inline struct timespec operator-(const struct timespec& spec1, const struct timespec& spec2)
{
  struct timespec spec;
  spec.tv_sec = spec1.tv_sec - spec2.tv_sec;
  if (spec2.tv_nsec > spec1.tv_nsec) {
    --spec.tv_sec;
    spec.tv_nsec = 1000000000 + spec1.tv_nsec - spec2.tv_nsec;
  } else
    spec.tv_nsec = spec1.tv_nsec - spec2.tv_nsec;
  return spec;
}

inline bool operator==(const struct timespec& spec1, const struct timespec& spec2)
{
  return (spec1.tv_sec == spec2.tv_sec) && (spec1.tv_nsec == spec2.tv_nsec);
}

inline bool operator!=(const struct timespec& spec1, const struct timespec& spec2)
{
  return !operator==(spec1, spec2);
}

class io_ctl_handler;

class io_dispatch
{
public:

  // 'num_threads' is the total number of threads that will be used
  // to dispatch io events on.  If 'use_this_thread' is true, then
  // this calling thread will be one of those threads and the expectation
  // is that the caller will call 'start', exactly once, some time
  // shortly after this constructor has returned.  This form is to
  // permit further initialzation after the caller has constructed
  // an io_dispatch
  io_dispatch(int num_threads, bool use_this_thread);
  
  // only valid to call if 'use_this_thread' was passed true to the ctor
  // this function will not return until some thread calls 'stop'
  void start()
  {
    epoll_loop();
  }
  
  // will stall until all background threads have stopped
  ~io_dispatch();
  
  void stop();
  
  // base class associated with the action taken when there
  // is io available on a watched file descriptor
  class handler
  {
  public:
    virtual void io_avail(io_dispatch& io_d, const struct epoll_event& event) = 0;
  };
  
  // This function will call the system epoll_ctl, using the given
  // events, hnd, and this io_dispatch's ep_fd_.  Whenever io is
  // available on the given fd (according to 'events'), the given
  // hnd->io_avail will be called.  For that call the 'io_d' parameter
  // will be this io_dispatch, and the 'event' parameter will be the
  // one filled out by epoll_wait
  void epoll_ctl(int op, int fd, uint32_t events, handler* hnd) const
  {
    struct epoll_event evt;
    evt.events = events;
    evt.data.ptr = hnd;
    if (::epoll_ctl(ep_fd_, op, fd, &evt) < 0)
      do_error("epoll_ctl(ep_fd_, " << op_string(op) << ", " << fd << ", &evt)");
  }
  
  // pause all io threads (other than the one calling this function
  // if it happens to be called from an io thread) and execute 'f'
  // while they are paused. Once 'f' returns resume all io threads.
  template<typename Fn>
  void while_paused(Fn f)
  {
    // if this is called from an io thread, then there
    // is one less io thread we have to pause
    bool is_io_thread = on_io_thread();
    
    std::unique_lock<std::mutex> lock(pause_mutex_);
    num_paused_threads_ = is_io_thread ? 1 : 0;
    
    // if there is only one io thread, and
    // we are called from that thread, then
    // we don't want to write a k_pause command
    if (num_paused_threads_ < num_threads_) {
      char cmd = k_pause;
      if (write(send_ctl_fd_,&cmd,1) != 1)
        do_error("write");
    }
    
    while (num_paused_threads_ != num_threads_)
      pause_cond_.wait(lock);
      
    f();
    
    num_paused_threads_ = 0;
    resume_cond_.notify_all();
  }
  
  // execute the given function once on each
  // of the io threads
  template<typename Fn>
  void on_each(Fn f)
  {
    // if this is called from an io thread, then there
    // is one less io thread we have to cause to execute f
    bool is_io_thread = on_io_thread();
      
    std::unique_lock<std::mutex> lock(pause_mutex_);
    num_paused_threads_ = is_io_thread ? 1 : 0;
    
    // if there is only one io thread, and
    // we are called from that thread, then
    // we don't want to write a k_on_each command
    if (num_paused_threads_ < num_threads_) {
      auto tc = new thread_caller<Fn>(f);
      char buf[1+sizeof(tc)];
      buf[0] = k_on_each;
      memcpy(&buf[1],&tc,sizeof(tc));
      if (write(send_ctl_fd_,&buf[0],sizeof(buf)) != sizeof(buf))
        do_error("write(send_ctl_fd_,&buf[0],sizeof(buf))");
    }
    
    while (num_paused_threads_ != num_threads_)
      pause_cond_.wait(lock);
      
    if (is_io_thread)
      f();

    num_paused_threads_ = 0;
    resume_cond_.notify_all();
  }
    
  // execute the given function on (exactly) one
  // of the io threads
  template<typename Fn>
  void on_one(Fn f)
  {
    // if this is an io thread we can just call it here
    if (on_io_thread())
      f();
    else {
      char buf[k_oo_command_buf_size];
      on_one_command(f,buf);
      if (write(send_ctl_fd_,&buf[0],sizeof(buf)) != sizeof(buf))
        do_error("write(send_ctl_fd_,&buf[0],sizeof(buf))");
    }
  }
  
  enum {
    k_oo_command_buf_size = 1+sizeof(void*)
  };
  
  template<typename Fn>
  void on_one_command(Fn f, char (&buf)[1+sizeof(void*)])
  {
    auto tc = new thread_caller<Fn>(f);
    buf[0] = k_on_one;
    memcpy(&buf[1],&tc,sizeof(tc));
  }
  
  class scheduled_task
  {
    friend class io_dispatch;
    struct timespec when_;    // set by io_dispatch::schedule_task
  public:
    virtual void exec() = 0;
  };
  
  void schedule_task(scheduled_task* task, const struct timespec& rel_when);
  bool remove_task(scheduled_task* task);
  
  int new_command_pipe();

private:
  std::string op_string(int op)
  {
    switch(op) {
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
      if (tid == io_thread_ids_[i]) {
        is_io_thread = true;
        break;
      }
    return is_io_thread;
  }
  
  struct thread_caller_
  {
  public:
    virtual ~thread_caller_() {}
    virtual void exec() = 0;
  };
  
  template<typename Fn>
  struct thread_caller : public thread_caller_
  {
  public:
    thread_caller(Fn fn)
      : fn_(fn)
    {}
    
    virtual void exec()
    {
      fn_();
    }
    
  private:
    Fn fn_;
  };

  enum {
    k_wake = 0,
    k_pause = 1,
    k_on_each = 2,
    k_on_one = 3
  };

  io_dispatch(const io_dispatch&);
  io_dispatch(io_dispatch&&);
  
  friend class io_ctl_handler;
  friend class io_timer_handler;
  
  void epoll_loop();
  void wake_next_thread();
  
  bool                      running_;
  int                       ep_fd_;
  int                       send_ctl_fd_;
  std::vector<io_ctl_handler*> io_ctl_handlers_;
  int                       timer_fd_;
  std::vector<std::thread>  io_threads_;
  std::vector<int>          io_thread_ids_;
  std::atomic_int           thread_init_index_;
  int                       num_threads_;
  
  std::mutex                pause_mutex_;
  std::condition_variable   pause_cond_;
  std::condition_variable   resume_cond_;
  int                       num_paused_threads_;
  
  std::mutex                                      task_mutex_;
  std::multimap<struct timespec,scheduled_task*>  task_map_;
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


