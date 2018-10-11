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

/*
  an "unmanaged" string, represented as a pointer
  and length.  This class simply makes the assumption that
  the memory pointed to by its pointer will outlive the
  class instance.  This makes it a very specialized class.
*/
struct string_len
{
  string_len()
      : str_(""),
        len_(0)
  {
  }

  string_len(const char *str, size_t len)
      : str_(str),
        len_(len)
  {
  }

  // warning! can only be called with a literal
  // or some other str whose lifespan exceeds
  // the lifespan of this string_len.
  explicit string_len(const char *str)
      : str_(str),
        len_(strlen(str))
  {
  }

  bool operator<(const string_len &sl) const
  {
    size_t l = len_ < sl.len_ ? len_ : sl.len_;
    auto ret = memcmp(str_, sl.str_, l);
    if (ret != 0)
      return ret < 0;
    return len_ < sl.len_;
  }

  std::string str() const { return std::string(str_, len_); }
  const char *ptr() const { return str_; }
  size_t len() const { return len_; }

private:
  const char *str_;
  size_t len_;
};

// helper
template <typename T>
T &operator<<(T &str, const string_len &sl)
{
  return str << sl.str();
}
