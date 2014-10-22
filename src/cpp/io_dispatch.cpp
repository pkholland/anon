
#include "io_dispatch.h"
#include "log.h"
#include <sys/epoll.h>
#include <system_error>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>

int                       io_dispatch::ep_fd_;
std::vector<std::thread>  io_dispatch::io_threads_;
bool                      io_dispatch::running_;
int                       io_dispatch::send_ctl_fd_;
int                       io_dispatch::recv_ctl_fd_;


class io_ctl_handler : public id_dispatch::handler
{
public:
  virtual void io_avail(const io_dispatch* io_d, const struct epoll_event& evt)
  {
    if (evt.events & EPOLLIN)
    {
      char cmd;
      if (read(io_d->recv_ctl_fd_, &cmd, 1) != 1)
        do_error("read");
      if (cmd == io_dispatch::k_wake)
        io_d->wake_next_thread();
      else if (cmd == io_dispatch::k_pause) {
        std::unique_lock<std::mutex> lock(io_d->mutex_);
        if (++io_d->paused_threads_ == id_d->io_threads_.size())
          io_d->pause_cond_.notify_one();
        else {
          char cmd = k_pause;
          write(io_d->send_ctl_fd_,&cmd,1);
        }
        while (id_d->paused_threads_ != 0)
          io_d->resume_cond_.wait(lock);
      }
    }
  }
};

static io_ctl_handler ctl_handler;

io_dispatch::io_dispatch(int num_threads)
  : running_(true)
{
  ep_fd_ = epoll_create1(EPOLL_CLOEXEC);
  if (ep_fd_ < 0)
    do_error("epoll_create1");
    
  anon_log("using fd " << ep_fd_ << " for epoll");
    
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sv) != 0) {
    close(ep_fd_);
    do_error("socketpair");
  }
  send_ctl_fd_ = sv[0];
  recv_ctl_fd_ = sv[1];
  
  // force recv_ctl_fd_ into EAGAIN state...
  char cmd;
  if ((read(recv_ctl_fd_, &cmd, 1) != -1) || (errno != EAGAIN))
    anon_log_error("initial read on recv_ctl_fd_ did not fail as expected");
    
  // now hook up the handler for the control pipe
  struct epoll_event evt;
  evt.events = EPOLLET | EPOLLIN;
  evt.data.fd = recv_ctl_fd_;
  evt.data.ptr = &ctl_handler;
  if (epoll_ctl(ep_fd_, EPOLL_CTL_ADD, recv_ctl_fd_, &evt) < 0)
    do_error("epoll_ctl");
  
  anon_log("using fds " << sv[0] << " and " <<  sv[1] << " for controlling epoll threads");
  
  for (int i = 0; i < num_threads; i++)
    io_threads_.push_back(std::bind(&io_dispatch::epoll_loop,this));
}

io_dispatch::~io_dispatch()
{
  running_ = false;
  wake_next_thread();
  for (auto thread = io_threads_.begin(); thread != io_threads_.end(); ++thread)
    thread->join();
  close(recv_ctl_fd_);
  close(send_ctl_fd_);
  close(ep_fd_);
}

void io_dispatch::epoll_ctl(int op, int fd, struct epoll_event* evt)
{
  if (epoll_ctl(ep_fd_, op, fd, evt) < 0)
    do_error("epoll_ctl");
}

void io_dispatch::wake_next_thread()
{
  char cmd = k_wake;
  if (write(send_ctl_fd_, &cmd, 1) != 1)
    do_error("write");
}

void io_dispatch::epoll_loop()
{
  anon_log("starting thread: io_dispatch::epoll_loop");

  while (true)  {
  
    struct epoll_event event;
    int ret;
    if ((ret = epoll_wait(ep_fd_, &event, 1, -1)) > 0) {
    
      ((handler*)event.data.ptr)->io_avail(*this, event);
      if (!running_) {
        wake_next_thread();
        break;
      }
    
    } else if ((ret != 0) && (errno != EINTR)) {
    
      // if we have a debugger attached, then every time the debugger hits a break point
      // it sends signals to all the threads, and we end up coming out of epoll_wait with
      // errno set to EINTR.  That's not worth printing...
      anon_log_error("epoll_wait returned error with errno: " << errno_string());
      
    }
  }
  
  anon_log("exiting thread: io_dispatch::epoll_loop");
}


