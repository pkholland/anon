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

#include <time.h>

inline struct timespec cur_time()
{
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    do_error("clock_gettime(CLOCK_MONOTONIC, &now)");
  return now;
}

inline bool operator<(const struct timespec& spec1, const struct timespec& spec2)
{
  if (spec1.tv_sec != spec2.tv_sec)
    return spec1.tv_sec < spec2.tv_sec;
  return spec1.tv_nsec < spec2.tv_nsec;
}

inline bool operator<=(const struct timespec& spec1, const struct timespec& spec2)
{
  return !operator<(spec2, spec1);
}

inline struct timespec operator+(const struct timespec& spec1, const struct timespec& spec2)
{
  struct timespec spec;
  spec.tv_sec = spec1.tv_sec + spec2.tv_sec;
  spec.tv_nsec = spec1.tv_nsec + spec2.tv_nsec;
  if (spec.tv_nsec > 1000000000) {
    ++spec.tv_sec;
    spec.tv_nsec -= 1000000000;
  }
  return spec;
}

inline struct timespec operator+(const struct timespec& spec1, int seconds)
{
  struct timespec spec = spec1;
  spec.tv_sec += seconds;
  return spec;
}

inline struct timespec operator-(const struct timespec& spec1, const struct timespec& spec2)
{
  struct timespec spec;
  spec.tv_sec = spec1.tv_sec - spec2.tv_sec;
  if (spec2.tv_nsec > spec1.tv_nsec) {
    --spec.tv_sec;
    spec.tv_nsec = 1000000000 + spec1.tv_nsec - spec2.tv_nsec;
  } else
    spec.tv_nsec = spec1.tv_nsec - spec2.tv_nsec;
  return spec;
}

inline struct timespec operator-(const struct timespec& spec1, int seconds)
{
  struct timespec spec = spec1;
  spec.tv_sec -= seconds;
  return spec;
}

inline bool operator==(const struct timespec& spec1, const struct timespec& spec2)
{
  return (spec1.tv_sec == spec2.tv_sec) && (spec1.tv_nsec == spec2.tv_nsec);
}

inline bool operator!=(const struct timespec& spec1, const struct timespec& spec2)
{
  return !operator==(spec1, spec2);
}

template<typename T>
T& operator<<(T& str, const struct timespec& spec)
{
  str << spec.tv_sec << ".";
  
  std::ostringstream dec;
  dec << std::setiosflags(std::ios_base::right) << std::setfill('0') << std::setw(3) << (spec.tv_nsec / 1000000);
  return str << dec.str();
}


