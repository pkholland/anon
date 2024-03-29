/*
 Copyright (c) 2015 ANON authors, see AUTHORS file.
 
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

#include "sproc_mgr.h"
#include "log.h"
#include <dirent.h>
#include <string.h>
#include <sys/stat.h>
#include <stdio.h>
#include <iomanip>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <string.h>
#include <thread>
#include <atomic>
#include <map>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <poll.h>

extern char **environ;

namespace
{

int listen_sock = -1;
int private_listen_sock = -1;
int current_srv_pid = 0;
std::vector<int> udps;

// glibc doesn't provide pidfd_open.  Documentation for
// it frequently discusses the need to directly call
// syscall exactly like this...
int pidfd_open(pid_t pid, unsigned int flags)
{
  return syscall(SYS_pidfd_open, pid, flags);
}

struct proc_info
{
  proc_info(const char *exe_name, bool do_tls, const std::vector<std::string> &args, const std::vector<std::string>& envs,
          const std::function<void()>& unexpected_restart)
      : exe_name_(exe_name),
        do_tls_(do_tls),
        args_(args),
        envs_(envs),
        exe_fd_(::open(exe_name, O_RDONLY)),
        unexpected_restart_(unexpected_restart)
  {
    if (exe_fd_ == -1)
      do_error("open(\"" << exe_name << "\", O_RDONLY)");
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, &cmd_pipe[0]) != 0)
    {
      ::close(exe_fd_);
      do_error("socketpair(AF_UNIX, SOCK_STREAM, 0, &cmd_pipe[0])");
    }
  }

  ~proc_info()
  {
    ::close(exe_fd_);
    ::close(cmd_pipe[0]);
    ::close(cmd_pipe[1]);
  }

  std::string exe_name_;
  std::vector<std::string> args_;
  std::vector<std::string> envs_;
  std::function<void()> unexpected_restart_;
  int exe_fd_;
  int cmd_pipe[2];
  bool do_tls_;
};

struct timeout_helper
{
  timeout_helper(int write_fd)
      : write_fd(write_fd),
        count(0)
  {
  }

  std::atomic_int count;
  int write_fd;
};

static void timeout_handler(union sigval sv)
{
  timeout_helper *to = (timeout_helper *)sv.sival_ptr;
  if (to->count++ == 0)
  {
    char reply = 0;
    if (::write(to->write_fd, &reply, 1))
    {
    }
  }
}

bool read_ok(int fd0, int fd1)
{
  // if the child process doesn't respond with an "ok\n" in one
  // second then our timer goes off and write "xx\n" into the pipe
  // causing the ::read call below to see that value.  We also
  // try to gracefully handle the case where the child process
  // and this timer go off at essentially the same time.
  timeout_helper to(fd1);

  struct sigevent sev = {0};
  sev.sigev_notify = SIGEV_THREAD;
  sev.sigev_notify_function = timeout_handler;
  sev.sigev_value.sival_ptr = &to;

  struct itimerspec its;
  its.it_value.tv_sec = 10000;
  its.it_value.tv_nsec = 0;
  its.it_interval.tv_sec = 0;
  its.it_interval.tv_nsec = 0;

  timer_t timerid;
  timer_create(CLOCK_MONOTONIC, &sev, &timerid);

  if (timer_settime(timerid, 0, &its, NULL) == -1)
    do_error("timer_settime(timerid, 0, &its, NULL)");

  char reply;
  if (::read(fd0, &reply, 1) <= 0)
    do_error("::read(fd0, &reply, 1)");

  timer_delete(timerid);

  // in the case where the timer goes off at essentially the same
  // time as the child process (finally) responded there is the possibility
  // that both answers will be in the pipe, and we want to clear all
  // readable data in the pipe in case we are going to try to use this
  // pipe again.
  if (to.count++ > 0)
  {
    if (fcntl(fd0, F_SETFL, fcntl(fd0, F_GETFL) | O_NONBLOCK) != 0)
      do_error("fcntl(fd0, F_SETFL, fcntl(pipe, F_GETFL) | O_NONBLOCK)");
    char buf[10];
    while (::read(fd0, &buf[0], sizeof(buf)) > 0)
    {
    }
    if (errno != EAGAIN)
      do_error("::read(fd0, &buf, 1)");
    if (fcntl(fd0, F_SETFL, fcntl(fd0, F_GETFL) & ~O_NONBLOCK) != 0)
      do_error("fcntl(fd0, F_SETFL, fcntl(pipe, F_GETFL) & ~O_NONBLOCK)");
    reply = 0;
  }

  return reply != 0;
}

#define k_start 0
#define k_stop 1
#define k_sync 2

bool write_cmd(int fd, char cmd)
{
  return ::write(fd, &cmd, 1) == 1;
}

void write_stop(int fd0, int fd1)
{
  if (write_cmd(fd0, k_stop))
    read_ok(fd0, fd1);
}

int start_child(proc_info &pi)
{
  auto pid = fork();
  if (pid == -1)
    do_error("fork()");

  if (pid != 0)
  {

    // here we are the (calling) parent, 'pid' is the child's process id
    // stall until the child echo's "ok\n" or it times out for whatever reason

    if (!read_ok(pi.cmd_pipe[0], pi.cmd_pipe[1]))
    {
      anon_log_error("child process " << pid << " started, but did not reply correctly, so was killed");
      kill(pid, SIGKILL);
      throw std::runtime_error("child process failed to start correctly");
    }
  }
  else
  {
    // we are the child, so fexecve to the child code
    close(pi.cmd_pipe[0]);

    std::vector<char *> args2;
    std::vector<char* > envs2;
    std::ostringstream oss;
    std::string oss_s;

    args2.push_back(const_cast<char *>(pi.exe_name_.c_str()));

    if (pi.do_tls_)
      args2.push_back(const_cast<char *>("-https_fd"));
    else
      args2.push_back(const_cast<char *>("-http_fd"));
    char lsock_buf[10];
    char psock_buf[10];
    sprintf(&lsock_buf[0], "%d", listen_sock);
    args2.push_back(&lsock_buf[0]);
    if (private_listen_sock != -1)
    {
      args2.push_back(const_cast<char *>("-private_fd"));
      sprintf(&psock_buf[0], "%d", private_listen_sock);
      args2.push_back(&psock_buf[0]);
    }

    if (udps.size() > 0)
    {
      args2.push_back(const_cast<char *>("-udp_fds"));
      auto is_first = true;
      for (auto p : udps)
      {
        if (!is_first)
          oss << ",";
        oss << p;
        is_first = false;
      }
      oss_s = oss.str();
      args2.push_back(const_cast<char *>(oss_s.c_str()));
    }

    // although these are going to be closed when we execute fexecve below,
    // we go ahead and close them here because they are low-numbered file
    // descriptors and that allows us to call dup on pi.cmd_pipe[1] causing
    // it to have a consistent, low number in the exec'ed process, letting us
    // reveal a little less about the state of this calling process.
    #if 0
    close(death_pipe[0]);
    close(death_pipe[1]);
    #endif

    args2.push_back(const_cast<char *>("-cmd_fd"));
    int new_pipe = dup(pi.cmd_pipe[1]);
    close(pi.cmd_pipe[1]);
    char cmd_pipe_buf[10];
    sprintf(&cmd_pipe_buf[0], "%d", new_pipe);
    args2.push_back(&cmd_pipe_buf[0]);

    for (int i = 0; i < pi.args_.size(); i++)
      args2.push_back(const_cast<char *>(pi.args_[i].c_str()));
    args2.push_back(0);

    auto e = &environ[0];
    while (*e != 0)
      envs2.push_back(*e++);
    for (auto &e : pi.envs_)
      envs2.push_back((char*)e.c_str());
    envs2.push_back(0);

    fexecve(pi.exe_fd_, &args2[0], &envs2[0]);

    // if fexecve succeeded then we never get here.  So getting here is a failure,
    // but we are already in the child process at this point, so we do what we can
    // to signifify the error and then exit
    fprintf(stderr, "fexecve(%d, ...) failed with errno: %d - %s\n", pi.exe_fd_, errno, strerror(errno));
    exit(1);
  }

  return pid;
}

std::map<int /*pid*/, std::unique_ptr<proc_info>> running_procs_map;
std::mutex running_procs_map_mutex;
std::condition_variable running_procs_map_cond;

void watch_for_child_death_and_restart(int child_pid)
{
  std::thread([child_pid] {
    auto pid_fd = pidfd_open(child_pid, 0);
    struct pollfd pfd = {0};
    pfd.fd = pid_fd;
    pfd.events = POLLIN;
    auto ready = poll(&pfd, 1, -1);
    if (ready > 0) {
      int exit_status;
      waitpid(child_pid, &exit_status, 0);
      std::unique_lock<std::mutex> lock(running_procs_map_mutex);
      auto p = running_procs_map.find(child_pid);
      if (p != running_procs_map.end()) {
        // if it is still in running_procs_map then we think
        // the process is supposed to be running, so
        // restart it
        anon_log_error("child process " << child_pid << " unexpectedly exited with exit_status: " << exit_status << ", restarting");
        auto pi = std::move(p->second);
        running_procs_map.erase(p);
        auto notify = pi->unexpected_restart_;
        auto new_chld = start_child(*pi);
        if (write_cmd(pi->cmd_pipe[0], k_start))
        {
        }
        running_procs_map.insert(std::make_pair(new_chld, std::move(pi)));
        if (notify)
          notify();
        watch_for_child_death_and_restart(new_chld);
      }
    }
    else {
      anon_log("strange, poll with no timeout specified timed out anyway");
    }
    close(pid_fd);
  }).detach();
}

} // namespace

void sproc_mgr_init(int port, int private_port, const std::vector<int> udp_ports, bool udp_is_ipv6)
{
  // no SOCK_CLOEXEC since we inherit this socket down to the child
  listen_sock = socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK, IPPROTO_TCP);
  if (listen_sock == -1)
    do_error("socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK, IPPROTO_TCP)");

  anon_log("using fd " << listen_sock << " for main listening socket");

  // bind to any address that will route to this machine
  struct sockaddr_in6 addr = {0};

  addr.sin6_family = AF_INET6;
  addr.sin6_port = htons(port);
  addr.sin6_addr = in6addr_any;
  if (bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0)
  {
    close(listen_sock);
    do_error("bind(" << listen_sock << ", <port: " << port << ", in6addr_any>, sizeof(addr))");
  }

  if (listen(listen_sock, SOMAXCONN) != 0)
  {
    close(listen_sock);
    do_error("listen(" << listen_sock << ", SOMAXCONN)");
  }

  if (private_port != 0)
  {
    private_listen_sock = socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK, IPPROTO_TCP);
    if (private_listen_sock == -1)
      do_error("socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK, IPPROTO_TCP)");

    anon_log("using fd " << private_listen_sock << " for private listening socket");

    // bind to any address that will route to this machine
    struct sockaddr_in6 addr = {0};

    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(private_port);
    addr.sin6_addr = in6addr_any;
    if (bind(private_listen_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0)
    {
      close(private_listen_sock);
      do_error("bind(" << private_listen_sock << ", <port: " << private_port << ", in6addr_any>, sizeof(addr))");
    }

    if (listen(private_listen_sock, SOMAXCONN) != 0)
    {
      close(private_listen_sock);
      do_error("listen(" << private_listen_sock << ", SOMAXCONN)");
    }
  }

  for (auto udp : udp_ports)
  {
    auto sock = socket(udp_is_ipv6 ? AF_INET6 : AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (sock == -1)
      do_error("socket(AF_INET6, SOCK_DGRAM | SOCK_NONBLOCK, 0)");

    // bind to any address that will route to this machine
    struct sockaddr_in6 addr = {0};
    socklen_t sz;
    if (udp_is_ipv6)
    {
      addr.sin6_family = AF_INET6;
      addr.sin6_port = htons(udp);
      addr.sin6_addr = in6addr_any;
      sz = sizeof(sockaddr_in6);
    }
    else
    {
      auto addr4 = (struct sockaddr_in*)&addr;
      addr4->sin_family = AF_INET;
      addr4->sin_port = htons(udp);
      addr4->sin_addr.s_addr = INADDR_ANY;
      sz = sizeof(sockaddr_in);
    }
    if (bind(sock, (struct sockaddr *)&addr, sz) != 0)
    {
      close(sock);
      do_error("bind(<AF_INET/6 SOCK_DGRAM socket>, <" << sock << ", in6addr_any/INADDR_ANY>, sizeof(...))");
    }
    udps.push_back(sock);
  }
}

void sproc_mgr_term()
{
  stop_server();
  close(listen_sock);
  listen_sock = -1;
  if (private_listen_sock != -1)
  {
    close(private_listen_sock);
    private_listen_sock = -1;
  }
  anon_log("sproc_mgr_term finished");
}

void start_server(const char *exe_name, bool do_tls, const std::vector<std::string> &args,
    const std::vector<std::string>& envs, const std::function<void()>& unexpected_restart)
{
  std::unique_ptr<proc_info> pi(new proc_info(exe_name, do_tls, args, envs, unexpected_restart));
  auto chld = start_child(*pi);

  std::unique_lock<std::mutex> lock(running_procs_map_mutex);
  auto p = running_procs_map.find(current_srv_pid);
  current_srv_pid = chld;
  if (p != running_procs_map.end()) {
    auto pp = std::move(p->second);
    running_procs_map.erase(p);
    write_stop(pp->cmd_pipe[0], pp->cmd_pipe[1]);
  }
  if (write_cmd(pi->cmd_pipe[0], k_start))
  {
  }
  running_procs_map.insert(std::make_pair(chld, std::move(pi)));
  watch_for_child_death_and_restart(chld);
}

void stop_server()
{
  std::unique_lock<std::mutex> lock(running_procs_map_mutex);
  auto p = running_procs_map.find(current_srv_pid);
  current_srv_pid = 0;
  if (p != running_procs_map.end()) {
    auto pp = std::move(p->second);
    running_procs_map.erase(p);
    write_stop(p->second->cmd_pipe[0], p->second->cmd_pipe[1]);
  }
}

void send_sync()
{
  std::unique_lock<std::mutex> lock(running_procs_map_mutex);
  auto p = running_procs_map.find(current_srv_pid);
  if (p != running_procs_map.end())
    write_cmd(p->second->cmd_pipe[0], k_sync);
}

int current_server_pid()
{
  return current_srv_pid;
}
