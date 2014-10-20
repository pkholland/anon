
#pragma once

#include <unistd.h>
#include <sys/syscall.h>
#include <sstream>
#include <ctime>
#include <time.h>
#include <chrono>
#include <iomanip>

#define anon_log(_body) Log::output(__FILE__, __LINE__, [&](std::ostream& formatter) { formatter << _body;})

namespace Log
{
  template<typename Func>
  static void output(const char* file_name, int line_num, Func func)
  {
    std::ostringstream format;

    // hour:minute:second.milli
    std::time_t t = std::time(nullptr);
    char mbstr[100];
    if (std::strftime(mbstr, sizeof(mbstr), "%H:%M:%S.", std::localtime(&t))) {
      format << mbstr;
      std::chrono::system_clock::time_point realtime = std::chrono::high_resolution_clock::now();
      format << (std::chrono::duration_cast<std::chrono::milliseconds>(realtime.time_since_epoch()).count() % 1000);
    }
   
    // (threadID, file_name, line_num)
    std::ostringstream loc;
    loc << " (" << syscall(SYS_gettid) << ", " << file_name << ", " << line_num << ")";
    format << std::setiosflags(std::ios_base::left) << std::setfill(' ') << std::setw(25) << loc.str();

    func(format);
    format << "\n";
    
    std::string s = format.str();
    write(1,s.c_str(),s.length());
  }
};

