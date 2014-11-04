
#pragma once

#include <mutex>

#if defined(ANON_RUNTIME_CHECKS)

namespace anon
{

void inc_lock_count();
void dec_lock_count();

template<typename T>
class unique_lock : public std::unique_lock<T>
{
public:
  unique_lock(T& t)
    : std::unique_lock<T>(t)
  {
    inc_lock_count();
  }
  
  ~unique_lock()
  {
    dec_lock_count();
  }
};

template<typename T>
class lock_guard : public std::lock_guard<T>
{
public:
  lock_guard(T& t)
    : std::lock_guard<T>(t)
  {
    inc_lock_count();
  }
  
  ~lock_guard()
  {
    dec_lock_count();
  }
};

void assert_no_locks();

}

#else

namespace anon
{
  using std::unique_lock;
  using std::lock_guard;
  
  inline void assert_no_locks(){}
}

#endif

