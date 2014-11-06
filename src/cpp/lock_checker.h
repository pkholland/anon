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

