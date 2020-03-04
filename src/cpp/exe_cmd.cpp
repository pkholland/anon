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
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0)
      do_error("socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv)");
  }

  ~sock_pr()
  {
    if (sv[0])
      close(sv[0]);
    if (sv[1])
      close(sv[1]);
  }

};

}

std::string exe_cmd_(const std::function<void(std::ostream &formatter)>& fn, bool first_line_only)
{
  sock_pr cmp;
  if (fcntl(cmp.sv[0], F_SETFL, fcntl(cmp.sv[0], F_GETFL) | O_NONBLOCK) != 0)
    do_error("fcntl(cmp.sv[0], F_SETFL, fnctl(cmp.sv[0], F_GETFL) | O_NONBLOCK)");
  std::unique_ptr<fiber_pipe> fp(new fiber_pipe(cmp.sv[0], fiber_pipe::unix_domain));
  cmp.sv[0] = 0;
  auto comp_pipe = cmp.sv[1];

  sock_pr iop;
  auto std_out = iop.sv[1];
  iop.sv[1] = 0;

  std::ostringstream cmd;
  fn(cmd);
  auto bash_cmd = cmd.str();
  std::string error_str;

  // fork, etc... are unhappy when executed from a fiber,
  // so create a new thread and do the work from that thread
  std::thread t([comp_pipe, std_out, bash_cmd, &error_str]{

    int iop[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, iop) != 0) {
      error_str = "socketpair(AF_UNIX, SOCK_STREAM, 0, iop)";
      int c = 1;
      write(comp_pipe, &c, sizeof(c));
      return;
    }

    auto pid = fork();
    if (pid == -1) {
      error_str = "fork()";
      close(iop[0]);
      close(iop[1]);
      int c = 1;
      write(comp_pipe, &c, sizeof(c));
      return;
    }
    if (pid != 0)
    {
      // the calling parent
      close(std_out);

      int status;
      auto w = waitpid(pid, &status, WUNTRACED | WCONTINUED);
      if (w == -1) {
        error_str = "waitpid(pid, &status, WUNTRACED | WCONTINUED)";
        int c = 1;
        write(comp_pipe, &c, sizeof(c));
      }
      else
      {
        int exit_code = 1;
        if (WIFEXITED(status))
          exit_code = WEXITSTATUS(status);
        else if (WIFSIGNALED(status))
          anon_log("bash killed by signal: " << WTERMSIG(status));
        else if (WIFSTOPPED(status))
          anon_log("bash stopped by signal: " << WSTOPSIG(status));
        error_str = "bash script failed";
        write(comp_pipe, &exit_code, sizeof(exit_code));
      }
    }
    else
    {
      // the child process
      dup2(std_out, 1); // reset stdout (1) to "std_out" 
      close(std_out);

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
    
  });

  // detach here.  We might come back from fp->read on a different
  // os thread, and we don't want thread's dtor trying to do anything
  // with the thread itself.
  t.detach();

  int exit_code;
  fp->read(&exit_code, sizeof(exit_code));
  if (exit_code != 0)
    anon_throw(std::runtime_error, error_str);

  std::ostringstream oss;
  char buff[1024];
  while (true) {
    auto bytes = read(iop.sv[0], &buff[0], sizeof(buff));
    if (bytes == 0)
      break;
    if (bytes < 0)
      do_error("read(iop.sv[0], &buff[0], sizeof(buff))");
    oss << std::string(&buff[0], bytes);
  }

  auto ret = oss.str();
  if (first_line_only)
    return ret.substr(0, ret.find("\n"));
  return ret;
}
