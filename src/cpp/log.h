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

#pragma once

#include <unistd.h>
#include <sys/syscall.h>
#include <sstream>
#include <ctime>
#include <time.h>
#include <chrono>
#include <iomanip>
#include <system_error>
#include <errno.h>
#include <string.h>

#if defined(ANON_LOG_FIBER_IDS)
extern int get_current_fiber_id();
#endif

#define anon_log(_body) Log::output(__FILE__, __LINE__, [&](std::ostream& formatter) {formatter << _body;}, false)
#define anon_log_error(_body) Log::output(__FILE__, __LINE__, [&](std::ostream& formatter) {formatter << _body;}, true)

namespace Log
{
  template<typename Func>
  static void output(const char* file_name, int line_num, Func func, bool err)
  {
    std::ostringstream format;

    // hour:minute:second.milli
    std::time_t t = std::time(nullptr);
    char mbstr[100];
    if (std::strftime(mbstr, sizeof(mbstr), "%H:%M:%S.", std::localtime(&t))) {
      format << mbstr;
      std::chrono::system_clock::time_point realtime = std::chrono::high_resolution_clock::now();
      std::ostringstream tm;
      tm << std::setiosflags(std::ios_base::right) << std::setfill('0') << std::setw(3);
      tm << (std::chrono::duration_cast<std::chrono::milliseconds>(realtime.time_since_epoch()).count() % 1000);
      format << tm.str();
    }
   
    // (threadID, file_name, line_num)
    std::ostringstream loc;
    loc << " (" << syscall(SYS_gettid);
    #if defined(ANON_LOG_FIBER_IDS)
    std::ostringstream fb;
    fb << ":";
    int fid = get_current_fiber_id();
    if (fid)
      fb << std::setiosflags(std::ios_base::right) << std::setfill('0') << std::setw(5) << get_current_fiber_id();
    else
      fb << ".....";
    loc << fb.str();
    const int width = 46;
    #else
    const int width = 40;
    #endif
    loc << ", " << file_name << ", " << line_num << ")";
    format << std::setiosflags(std::ios_base::left) << std::setfill(' ') << std::setw(width) << loc.str();

    // the "body" of the anon_log message
    func(format);
    format << "\n";
    
    // written to either stderr or stdout
    // test the return value of write to quite compiler warnings,
    // but there is nothing we can do if it fails for some reason.
    std::string s = format.str();
    if (write(err ? 2 : 1,s.c_str(),s.length()));
  }
};

inline std::string error_string2(int err)
{
  char  buff[256];
  return strerror_r(err, &buff[0], sizeof(buff));
}

inline std::string error_string1(int err)
{
  switch (err)
  {
    case EPERM:
      return "EPERM";
    case ENOENT:
      return "ENOENT";
    case EBADF:
      return "EBADF";
    case EACCES:
      return "EACCES";
    case ENOTDIR:
      return "ENOTDIR";
    case EROFS:
      return "EROFS";
    case EEXIST:
      return "EEXIST";
    case EAGAIN:
      return "EAGAIN";
    case EINVAL:
      return "EINVAL";
    case EMSGSIZE:
      return "EMSGSIZE";
    case EPROTOTYPE:
      return "EPROTOTYPE";
    case ENOPROTOOPT:
      return "ENOPROTOOPT";
    case EPROTONOSUPPORT:
      return "EPROTONOSUPPORT";
    case ESOCKTNOSUPPORT:
      return "ESOCKTNOSUPPORT";
    case EOPNOTSUPP:
      return "EOPNOTSUPP";
    case EPFNOSUPPORT:
      return "EPFNOSUPPORT";
    case EAFNOSUPPORT:
      return "EAFNOSUPPORT";
    case EADDRINUSE:
      return "EADDRINUSE";
    case EADDRNOTAVAIL:
      return "EADDRNOTAVAIL";
    case ENETDOWN:
      return "ENETDOWN";
    case ENETUNREACH:
      return "ENETUNREACH";
    case ENETRESET:
      return "ENETRESET";
    case ECONNABORTED:
      return "ECONNABORTED";
    case ECONNRESET:
      return "ECONNRESET";
    case ENOBUFS:
      return "ENOBUFS";
    case EISCONN:
      return "EISCONN";
    case ENOTCONN:
      return "ENOTCONN";
    case ESHUTDOWN:
      return "ESHUTDOWN";
    case ETOOMANYREFS:
      return "ETOOMANYREFS";
    case ETIMEDOUT:
      return "ETIMEDOUT";
    case ECONNREFUSED:
      return "ECONNREFUSED";
    case EHOSTDOWN:
      return "EHOSTDOWN";
    case EHOSTUNREACH:
      return "EHOSTUNREACH";
    default:
      return std::to_string(err);
    }
}

inline std::string error_string(int err)
{
  return std::string("(") + error_string1(err) + ") " + error_string2(err);
}

inline std::string errno_string()
{
  return error_string(errno);
}

#define do_error(fn) do {anon_log_error(fn << " failed with errno: " << errno_string()); throw std::system_error(errno, std::system_category());} while(0)


