#pragma once

#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>

class io_dispatch
{
public:

  io_dispatch(int num_threads);
  ~io_dispatch();
  
  class handler
  {
    virtual void io_avail(const io_dispatch& io_d, const struct epoll_event& event) = 0;
  };
  
  // This function will call ::epoll_ctl, using the given parameters
  // and this io_dispatch's ep_fd_.  It is a REQUIREMENT that the given
  // event->data.ptr point to an instance of a subclass of
  // 'handler'.  Whenever io is available on the given fd (according
  // to the event->events bits), event->data.ptr will be cast to a
  // handler* and its handler->io_avail will be called.  For that call
  // the io_d parameter will be this io_dispatch, and the event parameter
  // will be the one filled out by epoll_wait
  void epoll_ctl(int op, int fd, struct epoll_event* event) const;
  
  // pause all io threads and execute 'f' while they are paused.
  // Once 'f' returns resume all io threads.  Note, this _cannot_ be
  // called from one of the io_threads, otherwise it will dead lock.
  template<typename Fn>
  void while_paused(Fn f)
  {
    std::unique_lock<std::mutex> lock(mutex_);
    paused_threads_ = 0;
    char cmd = k_pause;
    write(send_ctl_fd_,&cmd,1);
    
    while (paused_threads_ != io_threads_.size())
      pause_cond_.wait(lock);
      
    f();
    
    paused_threads_ = 0;
    resume_cond_.notify_all();
  }

private:
  enum {
    k_wake = 0,
    k_pause = 1,
  };

  io_dispatch(const io_dispatch&);
  
  friend class io_ctl_handler;
  
  void epoll_loop();
  void wake_next_thread();
  
  bool running_;
  int ep_fd_;
  int send_ctl_fd_;
  int recv_ctl_fd_;
  std::vector<std::thread> io_threads_;
  
  std::mutex  mutex_;
  std::condition_variable paused_cond_;
  std::condition_variable resume_cond_;
  int paused_threads_;
};


