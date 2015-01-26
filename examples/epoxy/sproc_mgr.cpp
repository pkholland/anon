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

namespace {

int  listen_sock = -1;
int current_srv_pid = 0;
int death_pipe[2];
std::thread death_thread;

struct proc_info
{
  proc_info(const char* exe_name, const std::vector<std::string>& args)
    : exe_name_(exe_name),
      args_(args),
      exe_fd_(::open(exe_name, O_RDONLY))
  {
    if (exe_fd_ == -1)
      do_error("open(\"" << exe_name << "\", O_RDONLY)");
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, &cmd_pipe[0]) != 0) {
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
  
  std::string               exe_name_;
  std::vector<std::string>  args_;
  int                       exe_fd_;
  int                       cmd_pipe[2];
};

struct timeout_helper
{
  timeout_helper(int write_fd)
    : write_fd(write_fd),
      count(0)
  {}
  
  std::atomic_int count;
  int             write_fd;
  
};

static void timeout_handler(union sigval sv)
{
  timeout_helper *to = (timeout_helper*)sv.sival_ptr;
  if (to->count++ == 0) {
    char reply = 0;
    if (::write(to->write_fd, &reply, 1)) {}
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
  if (to.count++ > 0) {
    if (fcntl(fd0, F_SETFL, fcntl(fd0, F_GETFL) | O_NONBLOCK) != 0)
      do_error("fcntl(fd0, F_SETFL, fcntl(pipe, F_GETFL) | O_NONBLOCK)");
    char  buf[10];
    while (::read(fd0, &buf[0], sizeof(buf)) > 0) {}
    if (errno != EAGAIN)
      do_error("::read(fd0, &buf, 1)");
    if (fcntl(fd0, F_SETFL, fcntl(fd0, F_GETFL) & ~O_NONBLOCK) != 0)
      do_error("fcntl(fd0, F_SETFL, fcntl(pipe, F_GETFL) & ~O_NONBLOCK)");
    reply = 0;
  }
  
  return reply != 0;
}

#define k_start 0
#define k_stop  1

bool write_cmd(int fd, char cmd)
{
  return ::write(fd, &cmd, 1) == 1;
}

void write_stop(int fd0, int fd1)
{
  if (write_cmd(fd0, k_stop))
    read_ok(fd0, fd1);
}

int start_child(proc_info& pi)
{
  auto pid = fork();
  if (pid == -1)
    do_error("fork()");
  
  if (pid != 0) {

    // here we are the (calling) parent, 'pid' is the child's process id
    // stall until the child echo's "ok\n" or it times out for whatever reason

    if (!read_ok(pi.cmd_pipe[0], pi.cmd_pipe[1]))  {
      anon_log("child process " << pid << " started, but did not reply correctly, so was killed");
      kill(pid, SIGKILL);
      throw std::runtime_error("child process failed to start correctly");
    }
    
  } else {
  
    // we are the child, so fexecve to the child code
    close(pi.cmd_pipe[0]);
    
    std::vector<char*> args2;
    
    args2.push_back(const_cast<char*>(pi.exe_name_.c_str()));

    args2.push_back(const_cast<char*>("-listen_fd"));
    char lsock_buf[10];
    sprintf(&lsock_buf[0], "%d", listen_sock);
    args2.push_back(&lsock_buf[0]);
    
    // although these are going to be closed when we execute fexecve below,
    // we go ahead and close them here because they are low-numbered file
    // descriptors and that allows us to call dup on pi.cmd_pipe[1] causing
    // it to have a consistent, low number in the exec'ed process, letting us
    // reveal a little less about the state of this calling process.
    close(death_pipe[0]);
    close(death_pipe[1]);
    
    args2.push_back(const_cast<char*>("-cmd_fd"));
    int new_pipe = dup(pi.cmd_pipe[1]);
    close(pi.cmd_pipe[1]);
    char cmd_pipe_buf[10];
    sprintf(&cmd_pipe_buf[0], "%d", new_pipe);
    args2.push_back(&cmd_pipe_buf[0]);
    
    for (int i = 0; i < pi.args_.size(); i++)
      args2.push_back(const_cast<char*>(pi.args_[i].c_str()));
    
    args2.push_back(0);
    
    char* argp[1];
    argp[0] = 0;
    
    fexecve(pi.exe_fd_, &args2[0], &argp[0]);
    
    // if fexecve succeeded then we never get here.  So getting here is a failure,
    // but we are already in the child process at this point, so we do what we can
    // to signifify the error and then exit
    fprintf(stderr, "fexecve(%d, ...) failed with errno: %d - %s\n", pi.exe_fd_, errno, strerror(errno));
    exit(1);
    
  }
  
  return pid;
}


std::map<int/*pid*/, std::unique_ptr<proc_info>>  proc_map;
std::mutex                                        proc_map_mutex;

void handle_sigchld(int sig)
{
  pid_t chld;
  while ((chld=waitpid(-1, 0, WNOHANG)) > 0)
  {
    size_t tot_bytes = 0;
    char* data = (char*)&chld;
    while (tot_bytes < sizeof(chld)) {
      auto written = ::write(death_pipe[0], &data[tot_bytes], sizeof(chld)-tot_bytes);
      if (written < 0) {
        anon_log("couldn't write to death pipe");
        exit(1);
      }
      tot_bytes += written;
    } 
  }
}

}


void sproc_mgr_init(int port)
{
  // no SOCK_CLOEXEC since we inherit this socket down to the child
  listen_sock = socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK, IPPROTO_TCP);
  if (listen_sock == -1)
    do_error("socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK, IPPROTO_TCP)");
    
  anon_log("using fd " << listen_sock << " for main listening socket");

  // bind to any address that will route to this machine
  struct sockaddr_in6 addr = { 0 };
  
  addr.sin6_family = AF_INET6;
  addr.sin6_port = htons(port);
  addr.sin6_addr = in6addr_any;
  if (bind(listen_sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
    close(listen_sock);
    do_error("bind(" << listen_sock << ", <port: " << port << ", in6addr_any>, sizeof(addr))");
  }
  
  if (listen(listen_sock, SOMAXCONN) != 0) {
    close(listen_sock);
    do_error("listen(" << listen_sock << ", SOMAXCONN)");
  }
  
  if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, &death_pipe[0]) != 0) {
    close(listen_sock);
    do_error("socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, &death_pipe[0])");
  }
  
  death_thread = std::thread([]{
  
    while (true) {
    
      // read a pid for a child that has died
      pid_t chld;
      char* data = (char*)&chld;
      size_t tot_bytes = 0;
      while (tot_bytes < sizeof(chld)) {
        auto bytes_read = ::read(death_pipe[1], &data[tot_bytes], sizeof(chld)-tot_bytes);
        if (bytes_read <= 0) {
          anon_log("couldn't read from the death pipe");
          exit(1);
        }
        tot_bytes += bytes_read;
      }
      
      // you stop this thread by writting 0 into death_pipe[0];
      if (chld == 0)
        return;
        
      std::unique_lock<std::mutex> lock(proc_map_mutex);
      auto p = proc_map.find(chld);
      if (p == proc_map.end())
        anon_log("ignoring unregistered child process id: " << chld);
      else {
        auto pi = std::move(p->second);
        proc_map.erase(p);
        if (chld == current_srv_pid) {
          try {
            current_srv_pid = 0;
            auto new_chld = start_child(*pi);
            if (write_cmd(pi->cmd_pipe[0], k_start)) {}
            proc_map.insert(std::make_pair(new_chld,std::move(pi)));
            current_srv_pid = new_chld;
          } catch (const std::exception& err) {
            anon_log("caught exception: " << err.what());
          } catch (...) {
            anon_log("caught unknown exception trying to launch new server process");
          }
        }
      }
    }
    
  });
  
  struct sigaction sa;
  sa.sa_handler = &handle_sigchld;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
  if (sigaction(SIGCHLD, &sa, 0) == -1) {
    close(listen_sock);
    do_error("sigaction(SIGCHLD, &sa, 0)");
  }
}

void sproc_mgr_term()
{
  stop_server();
  close(listen_sock);
  listen_sock = -1;
  
  size_t tot_bytes = 0;
  pid_t chld = 0;
  char* data = (char*)&chld;
  while (tot_bytes < sizeof(chld)) {
    auto written = ::write(death_pipe[0], &data[tot_bytes], sizeof(chld)-tot_bytes);
    if (written < 0) {
      anon_log("couldn't write to death pipe");
      exit(1);
    }
    tot_bytes += written;
  }
  death_thread.join();
  
  close(death_pipe[0]);
  close(death_pipe[1]);
  
  std::unique_lock<std::mutex> lock(proc_map_mutex);
  for (auto chld = proc_map.begin(); chld != proc_map.end(); chld++)
    kill(chld->first, SIGKILL);
  proc_map = std::map<int/*pid*/, std::unique_ptr<proc_info>>();
}

void start_server(const char* exe_name, const std::vector<std::string>& args)
{
  std::unique_ptr<proc_info>  pi(new proc_info(exe_name, args));
  auto chld = start_child(*pi);
  
  std::unique_lock<std::mutex> lock(proc_map_mutex);
  auto p = proc_map.find(current_srv_pid);
  if (p != proc_map.end())
    write_stop(p->second->cmd_pipe[0], p->second->cmd_pipe[1]);
  current_srv_pid = chld;
  if (write_cmd(pi->cmd_pipe[0], k_start)) {}
  proc_map.insert(std::make_pair(chld,std::move(pi)));
}

void stop_server()
{
  std::unique_lock<std::mutex> lock(proc_map_mutex);
  auto p = proc_map.find(current_srv_pid);
  if (p != proc_map.end())
    write_stop(p->second->cmd_pipe[0], p->second->cmd_pipe[1]);
  current_srv_pid = 0;
}

int current_server_pid()
{
  return current_srv_pid;
}

void list_exes(const char* base_path, const char* name_match, std::ostringstream& reply)
{
  DIR* d = opendir(base_path);
  if (!d) {
    reply << "opendir(\"" << base_path << "\") failed with errno: " << errno << " - " << strerror(errno) << "\n";
    return;
  }
  
  struct dirent entry;
  struct dirent* entryp;
  int item = 0;
  
  reply << "\n" << name_match << " executables available in " << base_path << ":\n";
  
  while (true) {
    int res;
    if ((res = readdir_r(d, &entry, &entryp)) != 0) {
      reply << "readdir_r(d, &entry, &entryp) failed with errno: " << errno << " - " << strerror(errno) << "\n";
      break;
    }
    if (entryp == 0)
      break;
    if (strncmp(&entry.d_name[0], name_match, strlen(name_match)) == 0) {
      char  full_path[4096];
      strcpy(full_path,base_path);
      strcat(full_path,&entry.d_name[0]);
      struct stat st;
      if (stat(full_path, &st) != 0) {
        reply << "stat(\"" << full_path << ", &st) failed with errno: " << errno << " - " << strerror(errno) << "\n";
        break;
      }
      if (S_ISREG(st.st_mode) && ((st.st_mode & (S_IRUSR | S_IXUSR)) == (S_IRUSR | S_IXUSR))) {
        std::string command = std::string("sha1sum ") + full_path + "\n";
        FILE* ossl = popen(command.c_str(), "r");
        char md5_buf[1024];
        if (ossl == 0) {
          md5_buf[0] = '\n';
          md5_buf[1] = 0;
        } else {
          if (fread(&md5_buf[0], 1, sizeof(md5_buf), ossl)) {}
          char* p = &md5_buf[0];
          while (*p) {
            if (*p == ' ') {
              *p = 0;
              break;
            }
            ++p;
          }
          pclose(ossl);
        }
        std::ostringstream f;
        f << " " << ++item << ") " << &entry.d_name[0];
        reply << std::setiosflags(std::ios_base::left) << std::setfill(' ') << std::setw(20) << f.str() << "sha1: " << &md5_buf[0] << "\n";
      }
    }
  }
  
  reply << "\n";
  
  closedir(d);
}

std::string current_exe_name()
{
  std::unique_lock<std::mutex> lock(proc_map_mutex);
  auto p = proc_map.find(current_srv_pid);
  if (p != proc_map.end())
    return p->second->exe_name_;
  return "";
}


