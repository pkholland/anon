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

#include <string>
#include <streambuf>
#include <vector>

#if defined(ANON_AWS)
#include <aws/core/utils/StringUtils.h>
#endif

class pipe_t
{
public:
  virtual ~pipe_t() {}
  virtual size_t read(void *buff, size_t len) const = 0;
  virtual void write(const void *buff, size_t len) const = 0;
  virtual void limit_io_block_time(int seconds) = 0;
  virtual int get_fd() const = 0;
  virtual void set_hibernating(bool hibernating) = 0;
  virtual bool is_hibernating() const = 0;

  const pipe_t& operator<<(const char* str) const {
    write(str, strlen(str));
    return *this;
  }

  const pipe_t& operator<<(const std::string& str) const {
    write(&str.front(), str.size());
    return *this;
  }

  #if defined(ANON_AWS) && defined(USE_AWS_MEMORY_MANAGEMENT)
  // note, USE_AWS_MEMORY_MANAGEMENT gets turned on in some
  // aws sdk builds but not others.  When it is turned on
  // an Aws::String is not the same type as an std::string
  // (because its allocator is different).  This ends up
  // being kind of messy, but there are a few places where
  // it is much simpler to be able to insert any "string-like"
  // thing into a pipe, so in the case where an Aws::String
  // is not the same as an std::string, we want this extra
  // method to allow that insertion.  But if they are the
  // same type then we can't have this duplicated method.
  const pipe_t& operator<<(const Aws::String& str) const {
    write(&str.front(), str.size());
    return *this;
  }
  #endif

  const pipe_t& operator<<(std::streambuf* sb) const {
    const int sz = 1024 * 16;
    std::vector<char> buf(sz);
    while (true) {
      auto c = sb->sgetc();
      if (c == std::streambuf::traits_type::eof())
        break;
      sb->sputbackc(c);
      auto avail = sb->in_avail();
      while (avail > 0) {
        int chars = avail > sz ? sz : avail;
        auto read = sb->sgetn(&buf[0], sz);
        write(&buf[0], read);
        avail -= read;
      }
    }
    return *this;
  }
};

