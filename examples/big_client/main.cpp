/*
 Copyright (c) 2015 Anon authors, see AUTHORS file.
 
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

int retry(int& fd, struct addrinfo* addr_result, const char* buf, size_t len)
{
  //printf("getsockopt reported error on read, reconnecting\n");
  close(fd);
  fd = socket(addr_result->ai_addr->sa_family, SOCK_STREAM, 0);
  if (fd == -1) {
    printf("socket failed with errno: %d\n", errno);
    return 1;
  }
  if (connect(fd, addr_result->ai_addr, addr_result->ai_addrlen) != 0) {
    printf("connect failed with errno: %d\n", errno);
    return 1;
  }
  size_t tot_bytes = 0;
  while (tot_bytes<len) {
    auto bytes = write(fd,&buf[tot_bytes],len-tot_bytes);
    if (bytes < 0) {
      printf("write failed, errno: %d\n", errno);
      return 1;
    }
    tot_bytes += bytes;
  }
  return 0;
}

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
  
  const int num_sockets = 400;//400;
  const int num_sends = 2000;//2000;
  
  // look ip the addr for ip/port
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;  // use IPv4 or IPv6, whichever
  hints.ai_socktype = SOCK_STREAM;
  
  struct addrinfo* addr_result;
  if (getaddrinfo(ip,port,&hints,&addr_result)!=0) {
    printf("getaddrinfo failed with errno: %d\n", errno);
    return 1;
  }

  // loop through and get all the sockets connected
  std::vector<int> conns;
  for (int i=0; i<num_sockets; i++) {
    int fd = socket(addr_result->ai_addr->sa_family, SOCK_STREAM, 0);
    if (fd == -1) {
      printf("socket failed with errno: %d\n", errno);
      return 1;
    }
    if (connect(fd, addr_result->ai_addr, addr_result->ai_addrlen) != 0) {
      printf("connect failed with errno: %d\n", errno);
      return 1;
    }
    conns.push_back(fd);
  }
  
  // construct the GET request we are going to send
  // (it needs to have the Host attribute set right)
  std::ostringstream oss;
  oss << "GET /ims/profiles HTTP/1.1\r\n";
  oss << "Host: " << ip << ":" << port << "\r\n";
  oss << "User-Agent: big_client test agent\r\n";
  oss << "Accept: */*\r\n";
  oss << "\r\n";
  std::string st = oss.str();
  const char* buf = st.c_str();
  size_t len = st.length();
  
  auto start_time = cur_time();
  
  for (int send_count=0; send_count<num_sends; send_count++) {
    
    // send the request to each one
    for (int sock_count=0; sock_count<num_sockets; sock_count++) {
      int result;
      socklen_t optlen = sizeof(result);
      if (getsockopt(conns[sock_count], SOL_SOCKET, SO_ERROR, &result, &optlen) != 0) {
        printf("getsockopt failed, errno: %d\n", errno);
        return 1;
      }
      if (result != 0) {
        printf("getsockopt reported error %d on write\n", result);
        return 1;
      }
    
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
      int result;
      socklen_t optlen = sizeof(result);
      if (getsockopt(conns[sock_count], SOL_SOCKET, SO_ERROR, &result, &optlen) != 0) {
        printf("getsockopt failed, errno: %d\n", errno);
        return 1;
      }
      if (result != 0) {
        if (retry(conns[sock_count], addr_result, buf, len) != 0)
          return 1;
      }
      
      char    reply[4096];
      size_t  bytes_read = 0;
      while (true) {
        auto bytes = read(conns[sock_count],&reply[bytes_read],sizeof(reply)-bytes_read);
        if (bytes < 0) {
          if (retry(conns[sock_count], addr_result, buf, len) != 0)
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
  freeaddrinfo(addr_result);
  
  auto tot_time = cur_time() - start_time;
  std::ostringstream msg;
  msg << "tested " << num_sockets * num_sends << " http api calls in " << tot_time << " seconds\n";
  msg << "using " << num_sockets << " \"simutaneous\" sockets/clients, each making " << num_sends << " api calls\n";
  msg << num_sockets * num_sends / to_seconds(tot_time) << " api calls per second\n";
  printf("%s", msg.str().c_str());
  
  for (int i=0; i<num_sockets; i++)
    close(conns[i]);

  return 0;
}

