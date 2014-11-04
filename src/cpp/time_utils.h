
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


