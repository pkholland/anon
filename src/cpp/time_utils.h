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

#include <time.h>
#include "log.h"
#include <sstream>
#include <math.h>

inline struct timespec cur_time()
{
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    do_error("clock_gettime(CLOCK_MONOTONIC, &now)");
  return now;
}

inline struct timespec cur_epoc_time()
{
  struct timespec now;
  if (clock_gettime(CLOCK_REALTIME, &now) != 0)
    do_error("clock_gettime(CLOCK_REALTIME, &now)");
  return now;
}

// these next two are "finance inspired".  Given a time
// represented as a number of seconds since epoc, the first one
// returns the number of seconds since epoc for the time that
// is 'num_months' after the given time.  The idea is that if
// the given time is on the day of the 5th of some month, then
// the returned time is the 5th day of the next month.  These
// functions correctly account for different months having
// different numbers of days.  Also if the given time is on
// something like the 31st day of one month and the following
// month has less than 31 days, then the returned time is for
// the last day of that next month.  This includes correct
// handling of leap years.
inline time_t epoc_time_plus_n_months(time_t epoc_seconds, int num_months)
{
  struct tm t = {0};
  gmtime_r(&epoc_seconds, &t);

  auto months = t.tm_year * 12 + t.tm_mon + num_months;
  auto year = months / 12;
  auto month = months % 12;
  auto ndays_in_month = 30;
  switch(month) {
    case 1: {
      // careful with february
      auto full_year = year + 1900;
      auto is_leap_year = (full_year % 4) == 0 && ((full_year % 100) != 0 || (full_year % 400) == 0);
      ndays_in_month = is_leap_year ? 29 : 28;
    }  break;
    case 0:
    case 2:
    case 4:
    case 6:
    case 7:
    case 9:
    case 11:
      ndays_in_month = 31;
      break;
  }
  t.tm_mon = month;
  t.tm_year = year;
  if (t.tm_mday > ndays_in_month)
    t.tm_mday = ndays_in_month;
  return mktime(&t);
}

inline time_t epoc_time_plus_n_years(time_t epoc_seconds, int num_years)
{
  struct tm t = {0};
  gmtime_r(&epoc_seconds, &t);
  t.tm_year += num_years;

  if (t.tm_mon == 0 || t.tm_mday == 29) {
    auto full_year = t.tm_year + 1900;
    auto is_leap_year = (full_year % 4) == 0 && ((full_year % 100) != 0 || (full_year % 400) == 0);
    if (!is_leap_year)
      t.tm_mday = 28;
  }
  return mktime(&t);
}

inline bool operator<(const struct timespec &spec1, const struct timespec &spec2)
{
  if (spec1.tv_sec != spec2.tv_sec)
    return spec1.tv_sec < spec2.tv_sec;
  return spec1.tv_nsec < spec2.tv_nsec;
}

inline bool operator<=(const struct timespec &spec1, const struct timespec &spec2)
{
  return !operator<(spec2, spec1);
}

inline struct timespec operator+(const struct timespec &spec1, const struct timespec &spec2)
{
  struct timespec spec;
  spec.tv_sec = spec1.tv_sec + spec2.tv_sec;
  spec.tv_nsec = spec1.tv_nsec + spec2.tv_nsec;
  if (spec.tv_nsec > 1000000000)
  {
    ++spec.tv_sec;
    spec.tv_nsec -= 1000000000;
  }
  return spec;
}

inline struct timespec operator+(const struct timespec &spec1, int seconds)
{
  struct timespec spec = spec1;
  spec.tv_sec += seconds;
  return spec;
}

inline struct timespec operator+(const struct timespec &spec1, double seconds)
{
  struct timespec spec = spec1;
  double int_seconds;
  auto frac = std::modf(seconds, &int_seconds);
  spec.tv_sec += (int)int_seconds;
  spec.tv_nsec += (int)(1000000000 * frac);
  if (spec.tv_nsec < 0) {
    --spec.tv_sec;
    spec.tv_nsec += 1000000000;
  } else if (spec.tv_nsec >= 1000000000) {
    ++spec.tv_sec;
    spec.tv_nsec -= 1000000000;
  }
  return spec;
}


inline struct timespec operator-(const struct timespec &spec1, const struct timespec &spec2)
{
  struct timespec spec;
  spec.tv_sec = spec1.tv_sec - spec2.tv_sec;
  if (spec2.tv_nsec > spec1.tv_nsec)
  {
    --spec.tv_sec;
    spec.tv_nsec = 1000000000 + spec1.tv_nsec - spec2.tv_nsec;
  }
  else
    spec.tv_nsec = spec1.tv_nsec - spec2.tv_nsec;
  return spec;
}

inline struct timespec operator-(const struct timespec &spec1, int seconds)
{
  struct timespec spec = spec1;
  spec.tv_sec -= seconds;
  return spec;
}

inline bool operator==(const struct timespec &spec1, const struct timespec &spec2)
{
  return (spec1.tv_sec == spec2.tv_sec) && (spec1.tv_nsec == spec2.tv_nsec);
}

inline bool operator!=(const struct timespec &spec1, const struct timespec &spec2)
{
  return !operator==(spec1, spec2);
}

template <typename T>
T &operator<<(T &str, const struct timespec &spec)
{
  str << spec.tv_sec << ".";

  std::ostringstream dec;
  dec << std::setiosflags(std::ios_base::right) << std::setfill('0') << std::setw(3) << (spec.tv_nsec / 1000000);
  return str << dec.str();
}

inline double to_seconds(const struct timespec &spec)
{
  return spec.tv_sec + (spec.tv_nsec / 1000000000.);
}

inline struct timespec operator*(double m, const struct timespec &spec)
{
  struct timespec s;
  double i;
  s.tv_nsec = (int)(modf(to_seconds(spec) * m, &i) * 1000000000);
  s.tv_sec = (int)i;
  return s;
}

inline struct timespec operator*(const struct timespec &spec, double m)
{
  return m * spec;
}
