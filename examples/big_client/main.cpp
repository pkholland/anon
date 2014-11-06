/*
 Copyright (c) 2014 Anon authors, see AUTHORS file.
 
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

#include "time_utils.h"
#include <sys/socket.h>
#include <unistd.h>
#include <netdb.h>
#include <vector>
#include <sstream>
#include <string.h>

extern "C" int main(int argc, char** argv)
{
  if (argc != 3)
  {
    printf("usage: big_client <ip> <port>\n");
    return 1;
  }
  
  const char* ip = argv[1];
  const char* port = argv[2];
  printf("running big_client against \"%s\", port %s\n", ip, port);
  
  const int num_sockets = 100;
  const int num_sends = 1000;
  
  // look ip the addr for ip/port
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;  // use IPv4 or IPv6, whichever
  hints.ai_socktype = SOCK_STREAM;
  
  struct addrinfo* result;
  if (getaddrinfo(ip,port,&hints,&result)!=0) {
    printf("getaddrinfo failed with errno: %d\n", errno);
    return 1;
  }

  // loop through and get all the sockets connected
  std::vector<int> conns;
  for (int i=0; i<num_sockets; i++) {
    int fd = socket(result->ai_addr->sa_family, SOCK_STREAM, 0);
    if (fd == -1) {
      printf("socket failed with errno: %d\n", errno);
      return 1;
    }
    if (connect(fd, result->ai_addr, result->ai_addrlen) != 0) {
      printf("connect failed with errno: %d\n", errno);
      return 1;
    }
    conns.push_back(fd);
  }
  freeaddrinfo(result);
  
  // construct the GET request we are going to send
  // (it needs to have the Host attribute set right)
  std::ostringstream oss;
  oss << "GET /?a=10&b=20 HTTP/1.1\r\n";
  oss << "Host: " << ip << ":" << port << "\r\n";
  oss << "User-Agent: big_client test agent\r\n";
  oss << "Accept: text/html\r\n";
  oss << "Accept-Language: en-US,en;q-0.5\r\n";
  oss << "Accept-Encoding: gzip, deflate\r\n";
  oss << "Connection: keep-alive\r\n";
  oss << "\r\n\r\n";
  std::string st = oss.str();
  const char* buf = st.c_str();
  size_t len = st.length();
  
  auto start_time = cur_time();
  
  for (int send_count=0; send_count<num_sends; send_count++) {
  
    // send the request to each one
    for (int sock_count=0; sock_count<num_sockets; sock_count++) {
      size_t tot_bytes = 0;
      while (tot_bytes<len) {
        auto bytes = write(conns[sock_count],&buf[tot_bytes],len-tot_bytes);
        if (bytes < 0) {
          printf("write failed, errno: %d\n", errno);
          return 1;
        }
        tot_bytes += bytes;
      }
    }
    
    // read the response from each one
    for (int sock_count=0; sock_count<num_sockets; sock_count++) {
      char    reply[4096];
      size_t  bytes_read = 0;
      while (true) {
        auto bytes = read(conns[sock_count],&reply[bytes_read],sizeof(reply)-bytes_read);
        if (bytes < 0) {
          printf("read failed, errno: %d\n", errno);
          return 1;
        }
        bytes_read += bytes;
        if (bytes_read == sizeof(reply)) {
          printf("reply too big!\n");
          return 1;
        }
        if (bytes_read > 4 && memcmp(&reply[bytes_read-4],"\r\n\r\n",4) == 0)
          break;
      }
    }
    
  }
  
  auto tot_time = cur_time() - start_time;
  std::ostringstream msg;
  msg << "tested " << num_sockets * num_sends << " http api calls in " << tot_time << " seconds\n";
  msg << "using " << num_sockets << " \"simutaneous\" sockets/clients\n";
  msg << num_sockets * num_sends / to_seconds(tot_time) << " api calls per second\n";
  printf("%s", msg.str().c_str());
  
  for (int i=0; i<num_sockets; i++)
    close(conns[i]);

  return 0;
}

