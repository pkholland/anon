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

#include "io_dispatch.h"
#include "fiber.h"
#include "tls_context.h"
#include "big_id_crypto.h"
#include "dns_lookup.h"
#include "http_server.h"
#include "fiber.h"
#include "exe_cmd.h"

#ifdef ANON_AWS
#include "aws_client.h"
#endif

#include <dirent.h>
#include <fcntl.h>

// you have to supply an implementation of server_respond.
// (and server_init, server_term).  server_respond gets
// called every time there is a GET/POST/etc...
// http message sent to this server.

void server_init();
void server_respond(http_server::pipe_t &pipe, const http_request &request, bool is_tls);
void server_sync();
void server_term();
void server_close_outgoing();
std::vector<int> teflon_udp_ports_or_sockets;
bool teflon_udps_are_file_descriptors = false;


#define SERVER_STACK_SIZE 64 * 1024 - 128

static void show_help()
{
  printf("usage: teflon -http_fd <socket file descriptor number to use for listening for plain tcp connections>\n");
  printf("              or\n");
  printf("              -http_port <port number to listen on unencrypted>\n");
  printf("              and\n");
  printf("              -https_fd <socket file descriptor number to use for listening for tls tcp connections>\n");
  printf("              or\n");
  printf("              -https_port <port number to listen on encrypted>\n");
  printf("              and\n");
  printf("              -udp_ports <comma separated list of port numbers for udp ports>\n");
  printf("              or\n");
  printf("              -udp_fds <comma separated list of file descriptors for udp ports>\n");
  printf("              plus...\n");
  printf("              -cert_verify_dir <directory of trusted root certificates in c_rehash form>\n");
  printf("              -server_cert <certificate file for the server>\n");
  printf("              -server_key <private key file for the server's certificate>\n");
  printf("              -server_pw <OPTIONAL - password to decrypt server_key>\n");
  printf("              -cmd_fd <OPTIONAL - file descriptor number for the command pipe>\n");
}

static void get_ints(const std::string& arg, std::vector<int>& ints)
{
  std::size_t pos = 0;
  while (pos != std::string::npos)
  {
    auto next_pos = arg.find(",", pos);
    ints.push_back(atoi(arg.substr(pos, next_pos - pos).c_str()));
    pos = next_pos == std::string::npos ? next_pos : next_pos + 1;
  }
}

extern "C" int main(int argc, char **argv)
{
  bool port_is_fd = false;
  bool sport_is_fd = false;
  int http_port = -1;
  int https_port = -1;
  int cmd_pipe = -1;
  const char *cert_verify_dir = 0;
  const char *cert = 0;
  const char *key = 0;

  // all options are pairs for us, so there must
  // be an odd number of arguments (the first
  // one is the name of our executable).
  if (!(argc & 1))
  {
    show_help();
    return 1;
  }

  for (int i = 1; i < argc - 1; i++)
  {
    if (!strcmp("-http_fd", argv[i]))
    {
      http_port = atoi(argv[++i]);
      port_is_fd = true;
    }
    else if (!strcmp("-http_port", argv[i]))
    {
      http_port = atoi(argv[++i]);
    }
    else if (!strcmp("-https_fd", argv[i]))
    {
      https_port = atoi(argv[++i]);
      sport_is_fd = true;
    }
    else if (!strcmp("-https_port", argv[i]))
    {
      https_port = atoi(argv[++i]);
    }
    else if (!strcmp("-udp_ports", argv[i]))
    {
      get_ints(argv[++i], teflon_udp_ports_or_sockets);
    }
    else if (!strcmp("-udp_fds", argv[i]))
    {
      get_ints(argv[++i], teflon_udp_ports_or_sockets);
      teflon_udps_are_file_descriptors = true;
    }
    else if (!strcmp("-cert_verify_dir", argv[i]))
    {
      cert_verify_dir = argv[++i];
    }
    else if (!strcmp("-server_cert", argv[i]))
    {
      cert = argv[++i];
    }
    else if (!strcmp("-server_key", argv[i]))
    {
      key = argv[++i];
    }
    else if (!strcmp("-cmd_fd", argv[i]))
    {
      cmd_pipe = atoi(argv[++i]);
    }
    else
    {
      show_help();
      return 1;
    }
  }

  // did we get all the arguments we need?
  if (http_port <= 0 && https_port <= 0)
  {
    show_help();
    return 1;
  }

  if (https_port > 0 && (!cert_verify_dir || !cert || !key))
  {
    show_help();
    return 1;
  }

  anon_log("teflon server process starting");

  // initialize the io dispatch and fiber code
  // the last 'true' parameter says that we will be using
  // this calling thread as one of the io threads (after
  // we are done completing our initialization)
  auto num_threads =
#ifdef TEFLON_ONE_THREAD_ONLY
      1;
#else
      std::thread::hardware_concurrency();
#endif
  auto numSigFds =
#ifdef TEFLON_SIGFD
      TEFLON_SIGFD;
#else
      0;
#endif
  io_dispatch::start(num_threads, true, numSigFds);
  dns_lookup::start_service();
  fiber::initialize();
  init_big_id_crypto();
  exe_cmd_init();

  int ret = 0;
  try
  {

    // construct the server's ssl/tls context if we are
    // asked to use a tls port
    std::unique_ptr<tls_context> server_ctx;
    if (https_port > 0)
      server_ctx = std::unique_ptr<tls_context>(new tls_context(
          false /*client*/,
          0 /*verify_cert*/,
          cert_verify_dir,
          cert,
          key,
          5 /*verify_depth*/));

    // capture a closure which can be executed later
    // to create the http_servers, setting my_http
    // and/or my_https
    std::unique_ptr<http_server> my_http;
    std::unique_ptr<http_server> my_https;
    auto create_srvs_proc = [&] {

      #ifdef ANON_AWS
      aws_client_init();
      #endif

      server_init();

      if (https_port > 0)
        my_https = std::unique_ptr<http_server>(new http_server(https_port,
                                                                [](http_server::pipe_t &pipe, const http_request &request) {
                                                                  server_respond(pipe, request, true);
                                                                },
                                                                tcp_server::k_default_backlog, server_ctx.get(), sport_is_fd, SERVER_STACK_SIZE));

      if (http_port > 0)
        my_http = std::unique_ptr<http_server>(new http_server(http_port,
                                                               [](http_server::pipe_t &pipe, const http_request &request) {
                                                                 server_respond(pipe, request, false);
                                                               },
                                                               tcp_server::k_default_backlog, 0, port_is_fd, SERVER_STACK_SIZE));

    };

    // if we have been run by a tool capable of giving
    // us a command pipe, then hook up the parser for that.
    // Note that if we are directly executed from a shell
    // (for testing purposes) there will be no command pipe
    // and no way to communicate with the process.
    std::unique_ptr<fiber> cmd_fiber;
    if (cmd_pipe != -1)
    {

      fiber::run_in_fiber(
          [cmd_pipe, &my_http, &my_https, &cmd_fiber, &create_srvs_proc] {
            cmd_fiber = std::unique_ptr<fiber>(new fiber(
                [cmd_pipe, &my_http, &my_https, &create_srvs_proc] {
                  // the command parser itself
                  if (fcntl(cmd_pipe, F_SETFL, fcntl(cmd_pipe, F_GETFL) | O_NONBLOCK) != 0)
                    do_error("fcntl(cmd_pipe, F_SETFL, O_NONBLOCK)");

                  fiber_pipe pipe(cmd_pipe, fiber_pipe::unix_domain);
                  char cmd;
                  char ok = 1;

                  // tell the caller we are fully initialized and
                  // ready to accept commands
                  anon_log("ready to start http server");
                  pipe.write(&ok, sizeof(ok));

                  // continue to parse commands
                  // until we get one that tells us to stop
                  while (true)
                  {

                    try
                    {
                      pipe.read(&cmd, 1);
                    }
                    catch (const std::exception &err)
                    {
                      anon_log_error("command pipe unexpectedly failed: " << err.what());
                      exit(1);
                    }
                    catch (...)
                    {
                      anon_log_error("command pipe unexpectedly failed");
                      exit(1);
                    }
                    if (cmd == 0)
                    {
                      if (!my_http && !my_https)
                        create_srvs_proc();
                      else
                        anon_log_error("start command already processed");
                    }
                    else if (cmd == 1)
                      break;
                    else if (cmd == 2)
                      server_sync();
                    else
                      anon_log_error("unknown command: " << (int)cmd);
                  }

                  // tell the tcp server(s) to stop calling 'accept'
                  // these functions return once it is in a
                  // state of no longer calling accept on any thread
                  // and the listening socket is disconnected from the
                  // io dispatch mechanism, so no new thread will respond
                  // even if a client calls connect.
                  if (my_http)
                    my_http->stop();
                  if (my_https)
                    my_https->stop();
                  anon_log("http server stopped");

                  // tell the caller we have stopped calling accept
                  // on the listening socket(s).  It is now free to
                  // let some other process start calling it if it
                  // wishes.
                  pipe.write(&ok, sizeof(ok));

                  // tell the app to close any outgoing (they might be
                  // cached) connections, since the next step is to
                  // wait for all connections be to closed
                  io_params::sweep_hibernating_pipes();
                  server_close_outgoing();

                  // there may have been active network sessions because
                  // of previous calls to accept.  So wait here until
                  // they have all closed.  Note that in certain cases
                  // this may wait until the network io timeout has expired
                  fiber_pipe::wait_for_zero_net_pipes();

                  // instruct the io dispatch threads to all wake up and
                  // terminate.
                  io_dispatch::stop();
                },
                fiber::k_default_stack_size, false, "teflon main"));
          },
          fiber::k_default_stack_size, "teflon boot");
    }
    else
    {
      fiber::run_in_fiber(
          [&create_srvs_proc] {
            create_srvs_proc();
          },
          fiber::k_default_stack_size, "teflon direct");
    }

    // this call returns after the above call to io_dispatch::stop() has been
    // called -- meaning that some external command has told us to stop.
    // Or, if we are directly launched (with cmd_pipe == -1), then this
    // never returns and the process must be killed via external signal.
    io_dispatch::start_this_thread();

    // Whatever the app wants to do at termination time.
    //
    // At this point in time all network sockets that were
    // created as a consequence of clients calling
    // 'connect' to this server, have been closed
    server_term();

    exe_cmd_term();

    #ifdef ANON_AWS
    aws_client_term();
    #endif

    // shut down the dns_lookup thread
    dns_lookup::end_service();

    // wait for all io threads to terminate (other than this one)
    io_dispatch::join();

    // shut down the fiber control pipe mechanisms
    fiber::terminate();

    term_big_id_crypto();
  }
  catch (const std::exception &exc)
  {
    anon_log_error("caught exception: " << exc.what());
    ret = 1;
  }
  catch (...)
  {
    anon_log_error("caught unknown exception");
    ret = 1;
  }

  anon_log("teflon server process exiting");
  return ret;
}
