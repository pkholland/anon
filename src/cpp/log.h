
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
    loc << " (" << syscall(SYS_gettid) << ", ";
    #if defined(ANON_LOG_FIBER_IDS)
    loc << get_current_fiber_id() << ", ";
    #endif
    loc << file_name << ", " << line_num << ")";
    format << std::setiosflags(std::ios_base::left) << std::setfill(' ') << std::setw(40) << loc.str();

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

inline std::string errno_string()
{
  switch (errno)
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
    default:
      return std::to_string(errno);
    }
}

#define do_error(fn) do {anon_log_error(fn << " failed with errno: " << errno_string()); throw std::system_error(errno, std::system_category());} while(0)


