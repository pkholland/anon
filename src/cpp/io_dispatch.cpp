
#include "io_dispatch.h"
#include "log.h"
#include <sys/epoll.h>
#include <system_error>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>

class io_ctl_handler : public io_dispatch::handler
{
public:
  virtual void io_avail(io_dispatch& io_d, const struct epoll_event& evt)
  {
    if (evt.events & EPOLLIN)
    {
      char cmd;
      if (read(io_d.recv_ctl_fd_, &cmd, 1) != 1)
        do_error("read(io_d.recv_ctl_fd_, &cmd, 1)");
        
      // re-arm
      struct epoll_event evt;
      evt.events = EPOLLIN | EPOLLONESHOT;
      evt.data.ptr = this;
      io_d.epoll_ctl(EPOLL_CTL_MOD, io_d.recv_ctl_fd_, &evt);
        
      if (cmd == io_dispatch::k_wake)
        io_d.wake_next_thread();
        
      else if (cmd == io_dispatch::k_pause) {
      
        std::unique_lock<std::mutex> lock(io_d.mutex_);
        
        // if this is the last io thread to have paused
        // then signal whatever thread is waiting in
        // io_dispatch::while_paused, otherwise get the
        // next io thread to pause
        if (++io_d.num_paused_threads_ == io_d.num_threads_)
          io_d.pause_cond_.notify_one();
        else {
          char cmd = io_dispatch::k_pause;
          if (write(io_d.send_ctl_fd_,&cmd,1) != 1)
            do_error("write(io_d.send_ctl_fd_,&cmd,1)");
        }
        
        // wait until the thread that called io_dispatch::while_paused
        // to run its function and signal that everyone can
        // continue
        while (io_d.num_paused_threads_ != 0)
          io_d.resume_cond_.wait(lock);
          
      } else
        anon_log_error("unknown command (" << (int)cmd << ") written to control pipe - will be ignored" );
        
    } else
      anon_log_error("io_ctl_handler::io_avail called with event that does not have EPOLLIN set - control pipe now broken!");
  }
};

static io_ctl_handler ctl_handler;

io_dispatch::io_dispatch(int num_threads, bool use_this_thread)
  : running_(true),
    num_threads_(num_threads)
{
  ep_fd_ = epoll_create1(EPOLL_CLOEXEC);
  if (ep_fd_ < 0)
    do_error("epoll_create1(EPOLL_CLOEXEC)");
    
  anon_log("using fd " << ep_fd_ << " for epoll");
    
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sv) != 0) {
    close(ep_fd_);
    do_error("socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sv)");
  }
  send_ctl_fd_ = sv[0];
  recv_ctl_fd_ = sv[1];
  
  // force recv_ctl_fd_ into EAGAIN state...
  char cmd;
  if ((read(recv_ctl_fd_, &cmd, 1) != -1) || (errno != EAGAIN))
    anon_log_error("initial read on recv_ctl_fd_ did not fail as expected");
    
  // now hook up the handler for the control pipe
  // we use (level triggered) one-shot to give ourselves a bit
  // more control in getting the control pipe reads to happen
  // serially, and not just have all threads simultaneously
  // processing control commands.
  struct epoll_event evt;
  evt.events = EPOLLIN | EPOLLONESHOT;
  evt.data.ptr = &ctl_handler;
  if (::epoll_ctl(ep_fd_, EPOLL_CTL_ADD, recv_ctl_fd_, &evt) < 0)
    do_error("epoll_ctl(ep_fd_, EPOLL_CTL_ADD, recv_ctl_fd_, &evt)");
  
  anon_log("using fds " << sv[0] << " (send), and " <<  sv[1] << " (receive) for io threads control pipe");
  
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
  close(recv_ctl_fd_);
  close(send_ctl_fd_);
  close(ep_fd_);
}

void io_dispatch::stop()
{
  if (running_) {
    running_ = false;
    wake_next_thread();
  }
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


