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
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

char  base_path[4096];
char  cmd_path[4096];

// hard-coded for now
const char* exe_name;

static void validate_command_file()
{
  struct stat st;
  if (stat(cmd_path, &st) != 0) {
    if (errno == ENOENT) {
      if (mkfifo(cmd_path, 0666) != 0)
        do_error("mkfifo(\"" << &cmd_path[0] << "\", 0666");
      else
        return;
    } else
      do_error("stat(\"" << &cmd_path[0] << "\", &st)");
  }
  
  if (S_ISFIFO(st.st_mode))
    return;
  
  if (S_ISREG(st.st_mode)) {
    if (unlink(cmd_path) != 0)
      do_error("unlink(\"" << &cmd_path[0] << "\")");
    validate_command_file();
    return;
  } else if (S_ISDIR(st.st_mode)) {
    anon_log("\"" << &cmd_path[0] << "\" is a directory and must be manually deleted for this program to run");
    exit(1);
  }
  
  anon_log("\"" << &cmd_path[0] << "\" is an unknown file type and must be manually deleted for this program to run");
  exit(1);
}

static void write_all(int fd, const char* data)
{
  size_t  len = strlen(data);
  size_t  written = 0;
  while (written < len)
    written += ::write(fd,&data[written],len-written);
}

static bool process_command(const std::string& cmd)
{
  std::ostringstream  reply;
  bool                ret = true;
  bool                show_help = false;
  
  try {
    if (cmd == "help") {
    
      show_help = true;
      
    } else if (cmd == "quit") {
    
      reply << "\nquitting, bye\n\n";
      ret = false;
      
    } else if (cmd == "list_exes") {
    
      list_exes(base_path, exe_name, reply);
      
    } else if (cmd.find("start") == 0) {
    
      const char* p = &cmd.c_str()[5];
      while (*p && ((*p == ' ') || (*p == '\t')))
        ++p;
      std::string full_path = std::string(base_path) + p;
      
      // hard-coded for now...
      std::vector<std::string> args;
      args.push_back("-cert_verify_dir");
      args.push_back("/etc/ssl/certs");
      args.push_back("-server_cert");
      args.push_back("./secrets/srv_cert.pem");
      args.push_back("-server_key");
      args.push_back("./secrets/srv_key.pem");

      start_server(full_path.c_str(), args);
      reply << "\n" << p << " now running in process " << current_server_pid() << "\n\n";
      
    } else if (cmd == "current_exe") {
    
      if (current_server_pid())
        reply << "\ncurrent executable: " << current_exe_name() << ", in process id: " << current_server_pid() << "\n\n";
      else
        reply << "\nno executable currently running\n\n";
      
    } else {
    
      reply << "ignoring unknown command, you sent:\n" << cmd << "\n\n";
      show_help = true;
      
    }
  } catch (const std::exception& err) {
    reply << "\n\nerror: " << err.what() << "\n\n";
  } catch (...) {
    reply << "\n\nunknown error\n\n";
  }
  
  if (show_help) {
    reply << "available commands:\n\n";
    reply << "help\n";
    reply << "  shows this menu\n\n";
    reply << "quit\n";
    reply << "  quits the server application and all of its child processes\n\n";
    reply << "list_exes\n";
    reply << "  list the set of available executable images to run, along with their\n";
    reply << "  sha1 checksum values\n\n";
    reply << "start <executable name>\n";
    reply << "  starts the specified process running.  If there is already a process\n";
    reply << "  running it will perform a \"hot-swap\" of the process, stopping the\n";
    reply << "  older one and replacing it with the newer one\n\n";
    reply << "current_exe\n";
    reply << "  returns the file name and process id of the currently running executable\n";
    reply << "  if there is one, otherwise tells you that no process is currently running\n\n";
  }

  validate_command_file();
  int fd = open(cmd_path, O_WRONLY | O_CLOEXEC);
  if (fd != -1) {
    write_all(fd, reply.str().c_str());
    close(fd);
  }
  
  return ret;
}


extern "C" int main(int argc, char** argv)
{
  if (argc != 3) {
    fprintf(stderr,"usage: epoxy <port> <exe_name>\n");
    return 1;
  }
  
  size_t sz = strlen(argv[0]);
  if (sz > sizeof(cmd_path) - 20) {
    fprintf(stderr,"path to epoxy executable too long\n");
    return 1;
  }
  
  memcpy(base_path, argv[0], sz+1);
  char* p = &base_path[sz+1];
  while (p > &base_path[0] && *(p-1) != '/')
    --p;
  *p = 0;
  
  const char* cmd_file_name = ".epoxy_cmd";
  
  strcpy(cmd_path, base_path);
  strcat(cmd_path, cmd_file_name);
  
  int port = atoi(argv[1]);
  exe_name = argv[2];
  
  try {
    sproc_mgr_init(port);
  } catch (const std::exception& err) {
    anon_log("unable to initialize: " << err.what());
    return 1;
  } catch (...) {
    anon_log("unable to initialize");
    return 1;
  }
    
  anon_log("epoxy bound to network port " << port);
  anon_log("listening for commands on file " << &cmd_path[0]);
  
  int   exitcode = 0;
  char  cmd_buf[4096];

  validate_command_file();

  while (true) {

    int fd = open(cmd_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
      do_error("open(\"" << &cmd_path[0] << "\", O_RDONLY | O_CLOEXEC)");
    auto bytes = ::read(fd, &cmd_buf[0], sizeof(cmd_buf));
    close(fd);
    if (bytes == sizeof(cmd_buf)) {
      cmd_buf[20] = 0;
      anon_log("command too big, ignoring - starts with: \"" << &cmd_buf[0] << "...\"");
    }
    else {
      while (bytes > 0 && cmd_buf[bytes-1] == '\n')
        --bytes;
      if (bytes)  {
        bool keep_going = true;
        try {
          keep_going = process_command(std::string(&cmd_buf[0], 0, bytes));
        }
        catch(...)
        {}
        if (!keep_going)
          break;
      }
    }
    
  }
  
  unlink(cmd_path);
  sproc_mgr_term();
  
  anon_log("epoxy process exiting");
  return exitcode;

}


