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
#include "http_server.h"

#include <dirent.h>
#include <fcntl.h>

// you have to supply an implementation of server_respond.
// (and server_init, server_term).  server_respond gets
// called every time there is a GET/POST/etc...
// http message sent to this server.

void server_init();
void server_respond(http_server::pipe_t &pipe, const http_request &request, bool is_tls);
void server_term();
void server_close_outgoing();

#define SERVER_STACK_SIZE 64 * 1024 - 128

static void show_help()
{
  printf("usage: teflon -http_fd <socket file descript number to use for listening for plain tcp connections>\n");
  printf("              or\n");
  printf("              -http_port <port number to listen on unencrypted>\n");
  printf("              and\n");
  printf("              -https_fd <socket file descript number to use for listening for tls tcp connections\n");
  printf("              or\n");
  printf("              -https_port <port number to listen on encrypted\n");
  printf("              plus...\n");
  printf("              -cert_verify_dir <directory of trusted root certificates in c_rehash form>\n");
  printf("              -server_cert <certificate file for the server>\n");
  printf("              -server_key <private key file for the server's certificate>\n");
  printf("              -server_pw <OPTIONAL - password to decrypt server_key>\n");
  printf("              -cmd_fd <OPTIONAL - file descriptor number for the command pipe>\n");
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
  io_dispatch::start(std::thread::hardware_concurrency(), true);
  fiber::initialize();

  int ret = 0;
  try
  {

    server_init();

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
    auto create_srvs_proc = [&my_http, &my_https, &server_ctx, http_port, https_port, port_is_fd, sport_is_fd] {
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

      fiber::run_in_fiber([cmd_pipe, &my_http, &my_https, &cmd_fiber, &create_srvs_proc] {
        cmd_fiber = std::unique_ptr<fiber>(new fiber([cmd_pipe, &my_http, &my_https, &create_srvs_proc] {
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
              anon_log("command pipe unexpectedly failed: " << err.what());
              exit(1);
            }
            catch (...)
            {
              anon_log("command pipe unexpectedly failed");
              exit(1);
            }
            if (cmd == 0)
            {
              if (!my_http && !my_https)
                create_srvs_proc();
              else
                anon_log("start command already processed");
            }
            else if (cmd == 1)
              break;
            else
              anon_log("unknown command: " << (int)cmd);
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
          server_close_outgoing();

          // there may have been active network sessions because
          // of previous calls to accept.  So wait here until
          // they have all closed.  Note that in certain cases
          // this may wait until the network io timeout has expired
          fiber_pipe::wait_for_zero_net_pipes();

          // instruct the io dispatch threads to all wake up and
          // terminate.
          io_dispatch::stop();
        }));
      });
    }

    // this call returns after the above call to io_dispatch::stop() has been
    // called -- meaning that some external command has told us to stop.
    io_dispatch::start_this_thread();

    // Whatever the app wants to do at termination time.
    //
    // At this point in time all network sockets that were
    // created as a consequence of clients calling
    // 'connect' to this server, have been closed
    server_term();

    // wait for all io threads to terminate (other than this one)
    io_dispatch::join();

    // shut down the fiber control pipe mechanisms
    fiber::terminate();
  }
  catch (const std::exception &exc)
  {
    anon_log("caught exception: " << exc.what());
    ret = 1;
  }
  catch (...)
  {
    anon_log("caught unknown exception");
    ret = 1;
  }

  anon_log("teflon server process exiting");
  return ret;
}
