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

struct rez_file_ent
{
  const unsigned char *uncompressed;
  size_t sz_uncompressed;
  const unsigned char *compressed;
  size_t sz_compressed;
  const char *etag;
  const char *content_type;
};

const rez_file_ent *get_resource(const std::string &path);

class for_each_rez_helper
{
public:
  virtual ~for_each_rez_helper() {}
  virtual void rez(const std::string &path, const rez_file_ent *ent) = 0;
};

template <typename T>
class for_each_rez_helper_t : public for_each_rez_helper
{
public:
  for_each_rez_helper_t(T h)
      : _h(h)
  {
  }

  virtual void rez(const std::string &path, const rez_file_ent *ent) { _h(path, ent); }
  T _h;
};

void do_for_each_rez(for_each_rez_helper *h);

// t will be called with (const std::string& path, const rez_file_ent* ent) for each
// resource
template <typename T>
void for_each_rez(T t)
{
  do_for_each_rez(new for_each_rez_helper_t<T>(t));
}
