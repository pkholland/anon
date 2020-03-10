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

namespace {

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

int death_pipe[2];
fiber* death_fiber;
fiber_mutex mtx;
struct proc_exit {
  fiber_cond cond;
  bool finished;
  int status;
  proc_exit() : finished(false) {}
};
std::map<pid_t, proc_exit*> death_map;

struct proc_exit_status {
  pid_t pid;
  int status;
};

void handle_sigchld(int sig)
{
  proc_exit_status pes;
  while ((pes.pid = waitpid(-1, &pes.status, WNOHANG)) > 0)
  {
    size_t tot_bytes = 0;
    char *data = (char *)&pes;
    while (tot_bytes < sizeof(pes))
    {
      auto written = ::write(death_pipe[0], &data[tot_bytes], sizeof(pes) - tot_bytes);
      if (written < 0) {
        anon_log_error("couldn't write to death pipe");
        continue;
      }
      tot_bytes += written;
    }
  }
}

std::atomic_int cmd_count;

}

void exe_cmd_init()
{
  if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, &death_pipe[0]) != 0)
    do_error("socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, &death_pipe[0])");

  death_fiber = new fiber([]{
    if (fcntl(death_pipe[1], F_SETFL, fcntl(death_pipe[1], F_GETFL) | O_NONBLOCK) != 0)
      do_error("fcntl(death_pipe[1], F_SETFL, fnctl(death_pipe[1], F_GETFL) | O_NONBLOCK)");
    std::unique_ptr<fiber_pipe> fp(new fiber_pipe(death_pipe[1], fiber_pipe::unix_domain));
    death_pipe[1] = 0;

    while (true)
    {
      // read a pid for a child that has died
      proc_exit_status pes;
      char *data = (char *)&pes;
      size_t tot_bytes = 0;
      while (tot_bytes < sizeof(pes))
      {
        auto bytes_read = fp->read(&data[tot_bytes], sizeof(pes) - tot_bytes);
        if (bytes_read <= 0)
        {
          anon_log_error("couldn't read from the death pipe");
          return;
        }
        tot_bytes += bytes_read;
      }

      // you stop this fiber by writting 0,0 into death_pipe[0];
      if (pes.pid == 0)
        return;

      fiber_lock lock(mtx);
      auto p = death_map.find(pes.pid);
      if (p != death_map.end()) {
        p->second->finished = true;
        p->second->status = pes.status;
        p->second->cond.notify_all();
      }
    }

  });

  struct sigaction sa;
  sa.sa_handler = &handle_sigchld;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
  if (sigaction(SIGCHLD, &sa, 0) == -1)
    do_error("sigaction(SIGCHLD, &sa, 0)");
}

void exe_cmd_term()
{
  proc_exit_status pes = {0,0};
  ::write(death_pipe[0], &pes, sizeof(pes));
  death_fiber->join();
  delete death_fiber;
  ::close(death_pipe[0]);
}

std::string exe_cmd_(const std::function<void(std::ostream &formatter)>& fn, bool first_line_only)
{
  ++cmd_count;

  sock_pr iop;
  proc_exit pe;
  std::ostringstream cmd;
  fn(cmd);
  auto bash_cmd = cmd.str();
  std::ostringstream oss;

  {
    fiber_lock l(mtx);

    auto pid = fork();
    if (pid == -1)
      do_error("fork()");
    if (pid != 0)
    {
      // the calling parent
      iop.close(0);
      auto rfd = iop.sv[1];
      if (fcntl(rfd, F_SETFL, fcntl(rfd, F_GETFL) | O_NONBLOCK) != 0)
        do_error("fcntl(rfd, F_SETFL, fnctl(rfd, F_GETFL) | O_NONBLOCK)");
      iop.sv[1] = 0;
      std::unique_ptr<fiber> f(new fiber([rfd, &oss] {
        std::unique_ptr<fiber_pipe> fp(new fiber_pipe(rfd, fiber_pipe::unix_domain));
        char buff[1024];
        while (true) {
          try {
            auto bytes = fp->read(&buff[0], sizeof(buff));
            if (bytes == 0)
              break;
            if (bytes < 0)
              do_error("fp->read(&buff[0], sizeof(buff))");
            oss << std::string(&buff[0], bytes);
          }
          catch(...) {
            break;
          }
        }
      }));

      death_map[pid] = &pe;
      while(!pe.finished)
        pe.cond.wait(l);
      death_map.erase(pid);
      f->join();
    }
    else
    {
      // the child process
      dup2(iop.sv[0], 1); // reset stdout (1) to iop.sv[0] 
      iop.close(0);
      iop.close(1);

      auto bash_file_name = "/bin/bash";
      auto bash_fd = open(bash_file_name, O_RDONLY);
      if (bash_fd == -1) {
        fprintf(stderr, "open(%s, ...) failed with errno: %d - %s\n", bash_file_name, errno, strerror(errno));
        exit(1);
      }

      const char *dash_c = "-c";
      const char *script = bash_cmd.c_str();

      char *args[]{
          const_cast<char *>(bash_file_name),
          const_cast<char *>(dash_c),
          const_cast<char *>(script),
          0};

      fexecve(bash_fd, &args[0], environ);

      // if fexecve succeeded then we never get here.  So getting here is a failure,
      // but we are already in the child process at this point, so we do what we can
      // to signifify the error and then exit
      fprintf(stderr, "fexecve(%d, ...) failed with errno: %d - %s\n", bash_fd, errno, strerror(errno));
      exit(1);
    }
  }

  int exit_code = 1;
  if (WIFEXITED(pe.status))
    exit_code = WEXITSTATUS(pe.status);
  else if (WIFSIGNALED(pe.status))
    anon_log("bash killed by signal: " << WTERMSIG(pe.status));
  else if (WIFSTOPPED(pe.status))
    anon_log("bash stopped by signal: " << WSTOPSIG(pe.status));

  if (exit_code != 0)
    anon_throw(std::runtime_error, "bash script failed: " << bash_cmd);

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