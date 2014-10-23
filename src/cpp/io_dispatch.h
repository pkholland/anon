#pragma once

#include "log.h"
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <sys/epoll.h>

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
  
  class handler
  {
  public:
    virtual void io_avail(io_dispatch& io_d, const struct epoll_event& event) = 0;
  };
  
  // This function will call the system epoll_ctl, using the given
  // parameters and this io_dispatch's ep_fd_.  It is a REQUIREMENT that
  // the given event->data.ptr point to an instance of a subclass of
  // 'handler'.  Whenever io is available on the given fd (according
  // to the event->events bits), event->data.ptr will be cast to a
  // handler* and its handler->io_avail will be called.  For that call
  // the 'io_d' parameter will be this io_dispatch, and the 'event'
  // parameter will be the one filled out by epoll_wait
  void epoll_ctl(int op, int fd, struct epoll_event* event) const
  {
    if (::epoll_ctl(ep_fd_, op, fd, event) < 0)
      do_error("epoll_ctl(ep_fd_, " << op_string(op) << ", " << fd << ", event)");
  }
  
  // pause all io threads (other than the one calling this function
  // if it happens to be called from an io thread) and execute 'f'
  // while they are paused. Once 'f' returns resume all io threads.
  template<typename Fn>
  void while_paused(Fn f)
  {
    // if this is called from an io thread, then there
    // is one less io thread we have to pause
    bool is_io_thread = false;
    int tid = syscall(SYS_gettid);
    for (int i = 0; i < num_threads_; i++)
      if (tid == io_thread_ids_[i]) {
        is_io_thread = true;
        break;
      }
    
    std::unique_lock<std::mutex> lock(mutex_);
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

  enum {
    k_wake = 0,
    k_pause = 1,
  };

  io_dispatch(const io_dispatch&);
  io_dispatch(io_dispatch&&);
  
  friend class io_ctl_handler;
  
  void epoll_loop();
  void wake_next_thread();
  
  bool                      running_;
  int                       ep_fd_;
  int                       send_ctl_fd_;
  int                       recv_ctl_fd_;
  std::vector<std::thread>  io_threads_;
  std::vector<int>          io_thread_ids_;
  std::atomic_int           thread_init_index_;
  int                       num_threads_;
  
  std::mutex                mutex_;
  std::condition_variable   pause_cond_;
  std::condition_variable   resume_cond_;
  int                       num_paused_threads_;
};

inline std::string event_bits_to_string(uint32_t event_bits)
{
    std::string eventstr;
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


