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
#include <vector>
#include <mutex>
#if defined(__APPLE__)
#include <pthread.h>
#endif

#if defined(ANON_SYS_LOG)
#include <syslog.h>
#endif

#if defined(ANON_LOG_FIBER_IDS)
extern int get_current_fiber_id();
#endif

#if defined(ANON_LOG_KEEP_RECENT)
struct recent_logs
{
  recent_logs()
      : lines(num_kept),
        current(0)
  {
  }
  enum
  {
    num_kept = 1024
  };
  std::vector<std::string> lines;
  int current;
  static recent_logs singleton;
  std::mutex mutex;
};

inline void add_to_recent_logs(const std::string &line)
{
  std::unique_lock<std::mutex> l(recent_logs::singleton.mutex);
  auto indx = recent_logs::singleton.current % recent_logs::num_kept;
  recent_logs::singleton.lines[indx] = line;
  ++recent_logs::singleton.current;
}

template <typename T>
T &operator<<(T &t, const recent_logs &r)
{
  std::unique_lock<std::mutex> l(recent_logs::singleton.mutex);
  auto start_indx = r.current - recent_logs::num_kept;
  if (start_indx < 0)
    start_indx = 0;
  for (auto i = start_indx; i < r.current; i++)
    t << r.lines[i % recent_logs::num_kept];
  return t;
}
#endif

#define anon_log(_body) Log::output(__FILE__, __LINE__, [&](std::ostream &formatter) { formatter << _body; }, false)
#define anon_log_error(_body) Log::output(__FILE__, __LINE__, [&](std::ostream &formatter) { formatter << _body; }, true)

namespace Log
{
template <typename Func>
static void output(const char *file_name, int line_num, Func func, bool err)
{
  std::ostringstream format;

  // hour:minute:second.milli
  std::time_t t = std::time(nullptr);
  char mbstr[100];
  if (std::strftime(mbstr, sizeof(mbstr), "%H:%M:%S.", std::localtime(&t)))
  {
    format << mbstr;
    auto realtime = std::chrono::high_resolution_clock::now();
    std::ostringstream tm;
    tm << std::setiosflags(std::ios_base::right) << std::setfill('0') << std::setw(3);
    tm << (std::chrono::duration_cast<std::chrono::milliseconds>(realtime.time_since_epoch()).count() % 1000);
    format << tm.str();
  }

  // (threadID, file_name, line_num)
  std::ostringstream loc;
 #if defined(__APPLE__)
  uint64_t mac_tid;
  pthread_threadid_np(0, &mac_tid);
  loc << " (" << mac_tid;
 #else
  loc << " (" << syscall(SYS_gettid);
 #endif
#if defined(ANON_LOG_FIBER_IDS)
  std::ostringstream fb;
  fb << ":";
  int fid = get_current_fiber_id();
  if (fid)
    fb << std::setiosflags(std::ios_base::right) << std::setfill('0') << std::setw(5) << get_current_fiber_id();
  else
    fb << ".....";
  loc << fb.str();
  const int width = 60;
#else
  const int width = 54;
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
#if defined(ANON_LOG_KEEP_RECENT)
  add_to_recent_logs(s);
#endif
#if defined(ANON_SYS_LOG)
  syslog(err ? LOG_ERR : LOG_INFO, "%s", s.c_str());
#else
  if (write(err ? 2 : 1, s.c_str(), s.length()))
    ;
#endif
}
}; // namespace Log

inline std::string error_string2(int err)
{
  char buff[256];
  strerror_r(err, &buff[0], sizeof(buff));
  return &buff[0];
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

#if defined(ANON_LOG_ALL_THROWS)
#define anon_throw(_exc_type, _body)                                \
  do                                                                \
  {                                                                 \
    auto msg = Log::fmt([&](std::ostream &msg) { msg << _body; });  \
    anon_log(msg);                                                  \
    throw _exc_type(msg);                                           \
  } while (0)
#else
#define anon_throw(_exc_type, _body) throw _exc_type(Log::fmt([&](std::ostream &msg) { msg << _body; }))
#endif

#define do_error(fn)                                                \
  do                                                                \
  {                                                                 \
    anon_log_error(fn << " failed with errno: " << errno_string()); \
    throw std::system_error(errno, std::system_category());         \
  } while (0)

namespace Log
{
template <typename Func>
std::string fmt(Func func)
{
  std::ostringstream msg;
  func(msg);
  return msg.str();
}
}; // namespace Log
