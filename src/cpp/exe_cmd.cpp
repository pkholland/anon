/*
 Copyright (c) 2020 Anon authors, see AUTHORS file.
 
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

#include "exe_cmd.h"
#include <sstream>
#include "log.h"
#include "fiber.h"
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/syscall.h>      /* Definition of SYS_* constants */
#include <unistd.h>

namespace {

// glibc doesn't provide pidfd_open.  Documentation for
// it frequently discusses the need to directly call
// syscall exactly like this...
int pidfd_open(pid_t pid, unsigned int flags)
{
  return syscall(SYS_pidfd_open, pid, flags);
}

struct sock_pr {
  int sv[2];

  sock_pr()
  {
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
      do_error("socketpair(AF_UNIX, SOCK_STREAM, 0, sv)");
  }

  ~sock_pr()
  {
    if (sv[0])
      ::close(sv[0]);
    if (sv[1])
      ::close(sv[1]);
  }

  void close(int i)
  {
    ::close(sv[i]);
    sv[i] = 0;
  }

};

class child_proc_handler : public io_dispatch::handler
{
  bool& running_;
  fiber_mutex& mtx_;
  fiber_cond& cond_;

public:
  child_proc_handler(bool& running, fiber_mutex& mtx, fiber_cond& cond)
    : running_(running),
      mtx_(mtx),
      cond_(cond)
  {
  }

  ~child_proc_handler()
  {
  }

  virtual void io_avail(const struct epoll_event &evt)
  {
    if (evt.events & EPOLLIN)
    {
      fiber::run_in_fiber([this] {
        fiber_lock l(mtx_);
        running_ = false;
        cond_.notify_one();
      }, fiber::k_small_stack_size, "exe_cmd - io_avail");
    }
    else
      anon_log_error("child_proc_handler::io_avail called with event that does not have EPOLLIN set!");
  }

};

std::atomic_int cmd_count;

}


std::string exe_cmd_(const std::function<void(std::ostream &formatter)>& fn, bool first_line_only)
{
  ++cmd_count;

  sock_pr iop;
  std::ostringstream cmd;
  fn(cmd);
  auto bash_cmd = cmd.str();
  std::ostringstream oss;
  int exit_status = 0;

  auto bash_file_name = "/bin/bash";
  auto bash_fd = open(bash_file_name, O_RDONLY);
  if (bash_fd == -1) {
    do_error("open(\"/bin/bash\")");
  }

  const char *dash_c = "-c";
  const char *script = bash_cmd.c_str();
  char *args[]{
      const_cast<char *>(bash_file_name),
      const_cast<char *>(dash_c),
      const_cast<char *>(script),
      0};

  auto pid = fork();
  if (pid == -1) {
    do_error("fork()");
  }
  if (pid == 0) {
    // the child process
    dup2(iop.sv[0], 1); // reset stdout (1) to iop.sv[0]
    iop.close(0);
    iop.close(1);

    fexecve(bash_fd, &args[0], environ);

    // if fexecve succeeded then we never get here.  So getting here is a failure,
    // but we are already in the child process at this point, so we do what we can
    // to signifify the error and then exit
    fprintf(stderr, "fexecve(%d, ...) failed with errno: %d - %s\n", bash_fd, errno, strerror(errno));
    exit(1);
  }
  // else we are in the original/parent process here

  // close the bash fd
  close(bash_fd);

  // close the socket that the child will use for its stdout
  iop.close(0);

  // build a (non-blocking) fiber_pipe, allowing us to read
  // whatever the child writes into their stdout.  This will get
  // built up and stored into 'oss'
  auto rfd = iop.sv[1];
  if (fcntl(rfd, F_SETFL, fcntl(rfd, F_GETFL) | O_NONBLOCK) != 0)
    do_error("fcntl(rfd, F_SETFL, fnctl(rfd, F_GETFL) | O_NONBLOCK)");
  fiber_pipe read_pipe(rfd, fiber_pipe::unix_domain);
  iop.sv[1] = 0;

  // get a file descriptor that represents the process identified by 'pid'
  auto pid_fd = pidfd_open(pid, 0);

  // wait/watch for the process exit
  bool child_running = true;
  fiber_mutex child_running_mtx;
  fiber_cond child_running_cond;
  child_proc_handler child(child_running, child_running_mtx, child_running_cond);
  io_dispatch::epoll_ctl(EPOLL_CTL_ADD, pid_fd, EPOLLIN | EPOLLONESHOT, &child);

  // create and run the fiber that does this reading and recording
  bool fiber_running = true;
  fiber_mutex fiber_running_mtx;
  fiber_cond fiber_running_cond;
  fiber::run_in_fiber([&] {
    ((fiber*)get_current_fiber())->report_stack_usage();
    std::vector<char> buff(4096);
    while (true) {
      try {
        ((fiber*)get_current_fiber())->report_stack_usage();
        auto bytes = read_pipe.read(&buff[0], buff.size());
        ((fiber*)get_current_fiber())->report_stack_usage();
        if (bytes == 0)
          break;
        if (bytes < 0)
          do_error("read_pipe.read(&buff[0], buff.size())");
        oss << std::string(&buff[0], bytes);
      }
      catch(const fiber_io_error&)
      {
        break;
      }
      catch(...) {
        anon_log("caught unknown error reading from exe_cmd pipe");
        break;
      }
    }
    fiber_lock l(fiber_running_mtx);
    fiber_running = false;
    fiber_running_cond.notify_all();
  }, fiber::k_small_stack_size, "exe_cmd");

  {
    fiber_lock l(child_running_mtx);
    while (child_running) {
      child_running_cond.wait(l);
    }
  }
  io_dispatch::epoll_ctl(EPOLL_CTL_DEL, pid_fd, 0, &child);

  {
    fiber_lock l(fiber_running_mtx);
    while (fiber_running) {
      fiber_running_cond.wait(l);
    }
  }

  // at this point the child process is no longer running, we can now call waitpid
  // to get the exit status info and reap the child's process record
  waitpid(pid, &exit_status, 0);
  close(pid_fd);

  int exit_code = 1;
  if (WIFEXITED(exit_status))
    exit_code = WEXITSTATUS(exit_status);
  else if (WIFSIGNALED(exit_status))
    anon_log("bash killed by signal: " << WTERMSIG(exit_status));
  else if (WIFSTOPPED(exit_status))
    anon_log("bash stopped by signal: " << WSTOPSIG(exit_status));

  if (exit_code != 0)
    anon_throw(std::runtime_error, "bash script failed: \"" << bash_cmd << "\"");

  auto ret = oss.str();
  if (first_line_only)
    return ret.substr(0, ret.find("\n"));
  return ret;
}

void reset_exe_cmd_count()
{
  cmd_count = 0;
}

int get_exe_cmd_count()
{
  return cmd_count;
}