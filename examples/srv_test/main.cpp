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

#include "log.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

char  base_path[4096];
char  cmd_path[4096];

extern "C" int main(int argc, char** argv)
{
  if (argc != 2) {
    fprintf(stderr,"usage: srv_test <num of server restarts>\n");
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
  
  int num_restarts = atoi(argv[1]);

  anon_log("restarting " << num_restarts << " times");
  for (int i = 0; i < num_restarts; i++) {
  
    int fd = open(cmd_path, O_WRONLY);
    if (fd < 0)
      do_error("open(\"" << &cmd_path[0] << "\", O_WRONLY)");
    const char* cmd = "start teflon\n";
    if (write(fd, cmd, strlen(cmd)) != strlen(cmd))
      do_error("write(fd, cmd, strlen(cmd))");
    close(fd);
    
    fd = open(cmd_path, O_RDONLY);
    if (fd < 0)
      do_error("open(\"" << &cmd_path[0] << "\", O_RDONLY)");
    char reply[4096];
    ssize_t bytes;
    while ((bytes = ::read(fd, &reply[0], sizeof(reply))) > 0)
    close(fd);

  }
  anon_log("done restarting " << num_restarts << " times");
  
  return 0;
}


