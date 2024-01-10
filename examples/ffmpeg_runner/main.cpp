/*
 Copyright (c) 2024 ANON authors, see AUTHORS file.
 
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

#include <netdb.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <thread>
#include "log.h"
#include "worker_message.pb.h"

namespace {

int udp_sock = -1;
sockaddr_in6 udp_addr;
size_t udp_addr_sz;
std::string task_id;
std::string worker_id;
int progress_pipe[2];
int total_frames;

void init_udp_socket(const std::string& host, int port)
{
  struct addrinfo hints = {};
  hints.ai_family = AF_UNSPEC; // use IPv4 or IPv6, whichever
  hints.ai_socktype = SOCK_STREAM;
  char portString[8];
  sprintf(&portString[0], "%d", port);
  struct addrinfo *result;
  auto err = getaddrinfo(host.c_str(), &portString[0], &hints, &result);
  if (err == 0)
  {
    auto ipv6 = result->ai_addr->sa_family == AF_INET6;
    udp_addr_sz = ipv6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
    memcpy(&udp_addr, result->ai_addr, udp_addr_sz);
    freeaddrinfo(result);

    udp_sock = socket(ipv6 ? AF_INET6 : AF_INET, SOCK_DGRAM, 0);
    if (udp_sock == -1)
      do_error("socket(AF_INET6, SOCK_DGRAM, 0)");

    // bind to any address that will route to this machine
    struct sockaddr_in6 addr = {0};
    socklen_t sz;
    if (ipv6)
    {
      addr.sin6_family = AF_INET6;
      addr.sin6_port = 0;
      addr.sin6_addr = in6addr_any;
      sz = sizeof(sockaddr_in6);
    }
    else
    {
      auto addr4 = (struct sockaddr_in*)&addr;
      addr4->sin_family = AF_INET;
      addr4->sin_port = 0;
      addr4->sin_addr.s_addr = INADDR_ANY;
      sz = sizeof(sockaddr_in);
    }
    if (bind(udp_sock, (struct sockaddr *)&addr, sz) != 0)
    {
      close(udp_sock);
      udp_sock = -1;
      do_error("bind(<AF_INET/6 SOCK_DGRAM socket>, <" << 0 << ", in6addr_any/INADDR_ANY>, sizeof(...))");
    }
    else {
    }
  }
}

bool send_udp_message(const resin_worker::Message& msg)
{
  std::string bytes;
  if (msg.SerializeToString(&bytes)) {
    if (::sendto(udp_sock, bytes.c_str(), bytes.size(), 0, (sockaddr*)&udp_addr, udp_addr_sz) == -1) {
      anon_log("sendto failed with errno: " << errno_string() << ", msg size: " << bytes.size());
      return false;
    }
  }
  else {
    anon_log("msg.SerializeToString failed");
    return false;
  }
  return true;
}

void show_help(int argc, char** argv)
{
  printf("usage: ffmpeg_runner -status_udp_host <url to host listening for status updates over udp>\n");
  printf("              -status_udp_port <port number that host is listening on>\n");
  printf("              -task_id <id for this task>\n");
  printf("              -worker_id <id for this worker>\n");
  printf("              - <followed by parameters that should be passed to ffmpeg>\n");
  for (int i = 0; i < argc; i++) {
    printf("%s", argv[i]);
    if (i < argc - 1)
    {
      printf(" ");
    }
  }
  printf("\n");
}

void process_progress(const char* data)
{
  auto pos = strstr(data, "frame=");
  if (pos) {
    total_frames = atoi(pos + 6);
    resin_worker::Message msg;
    msg.set_message_type(resin_worker::Message_MessageType::Message_MessageType_TASK_STATUS);
    auto ts = msg.mutable_task_status();
    ts->set_worker_id(worker_id);
    ts->set_task_id(task_id);
    ts->set_cpu_count(std::thread::hardware_concurrency());
    ts->set_completed(0.0f);
    ts->set_completed_items(total_frames);
    ts->set_complete(false);
    send_udp_message(msg);
  }
}

}

extern "C" int main(int argc, char** argv)
{
  const char* udp_host = nullptr;
  int udp_port = 0;
  char** ffparams = nullptr;

  for (auto i = 1; i < argc; i++) {
    if (!strcmp("-status_udp_host", argv[i]) && i < argc - 1) {
      udp_host = argv[++i];
    }
    else if (!strcmp("-status_udp_port", argv[i]) && i < argc - 1) {
      udp_port = atoi(argv[++i]);
    }
    else if (!strcmp("-task_id", argv[i]) && i < argc - 1) {
      task_id = argv[++i];
    }
    else if (!strcmp("-worker_id", argv[i]) && i < argc - 1) {
      worker_id = argv[++i];
    }
    else if (!strcmp("-", argv[i])) {
      ffparams = &argv[i+1];
      break;
    }
  }

  if (!udp_host || !udp_port || worker_id.empty() || task_id.empty() || !ffparams) {
    show_help(argc, argv);
    exit(1);
  }

  auto ff = popen("which ffmpeg", "r");
  char ff_loc[1024];
  auto sz = fread(&ff_loc[0], 1, sizeof(ff_loc), ff);
  while (sz > 0 && ff_loc[sz-1] == '\n') {
    --sz;
  }
  ff_loc[sz] = 0;
  if (strlen(&ff_loc[0]) == 0) {
    printf("unable to find ffmpeg\n");
    exit(1);
  }

  try {
    init_udp_socket(udp_host, udp_port);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, &progress_pipe[0]) != 0)
    {
      do_error("socketpair(AF_UNIX, SOCK_STREAM, 0, &progress_pipe[0])");
    }
    auto pid = fork();
    if (pid == -1) {
      do_error("fork failed");
    }
    if (pid != 0) {
      // here we are the (calling) parent, 'pid' is the child's process id
      // the child has a copy of progress_pipe[1], so close our copy
      close(progress_pipe[1]);

      // read from progress_pipe[0] until progress_pipe[1] closes
      std::vector<char> buff(1024*16);
      while (true) {
        auto rd = read(progress_pipe[0], &buff[0], buff.size() - 1);
        if (rd <= 0) {
          break;
        }
        buff[rd] = 0;
        process_progress(&buff[0]);
      }
    }
    else {
      // here we are the child, close progress_pipe[0] and exec to ffmpeg
      close(progress_pipe[0]);

      // the parent uses its stdout to communicate back, and ffmpeg
      // can, in some cases also attempt to use std out for various
      // purposes.  We don't support those use cases and don't want
      // ffmpeg's stdout to interfere with ffmpeg_runner's stdout.
      // So redirect stdout to /dev/null.  Note that much of ffmpeg's
      // "logging" actually goes out to stderr (2) - not stdout (1)
      auto n = open("/dev/null", O_WRONLY);
      dup2(n, 1);

      std::vector<char *> args2;

      args2.push_back(&ff_loc[0]);
      std::string hide_banner("-hide_banner");
      args2.push_back(hide_banner.data());
      std::string progress("-progress");
      args2.push_back(progress.data());
      std::ostringstream pipe_oss;
      pipe_oss << "pipe:" << progress_pipe[1];
      auto pipe_arg = pipe_oss.str();
      args2.push_back(pipe_arg.data());
      std::string loglevel("-loglevel");
      std::string quiet("quiet");
      args2.push_back(loglevel.data());
      args2.push_back(quiet.data());
      while (ffparams < &argv[argc]) {
        args2.push_back(*ffparams);
        ++ffparams;
      }
      args2.push_back(0);

      execve(&ff_loc[0], &args2[0], &environ[0]);

      fprintf(stderr, "execve(ffmpeg, ...) failed with errno: %d - %s\n", errno, strerror(errno));
      exit(1);
    }
    anon_log("ffmpeg_runner:total_frames=" << total_frames);
    return 0;
  }
  catch(...) {}
  return 1;
}
