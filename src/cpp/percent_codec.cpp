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

#include "percent_codec.h"
#include <string.h>
#include <stdexcept>

static bool valid_hex(int c)
{
  return ((c >= '0') && (c <= '9')) || ((c >= 'a') && (c <= 'f')) || ((c >= 'A') && (c <= 'F'));
}

static int nib(int c)
{
  if ((c >= '0') && (c <= '9'))
    return c - '0';
  if ((c >= 'a') && (c <= 'f'))
    return c - 'a' + 10;
  return c - 'A' + 10;
}

std::string percent_decode(const std::string &encoded)
{
  std::string dec;
  const char *enc = encoded.c_str();
  while (*enc)
  {
    auto pos = strchrnul(enc, '%');
    dec += std::string(enc, pos - enc);
    if (*pos == '%')
    {
      if (!valid_hex(pos[1]) || !valid_hex(pos[2]))
        throw std::runtime_error("invalid encoded uri character");
      char c[2];
      c[0] = nib(pos[1]) * 16 + nib(pos[2]);
      c[1] = 0;
      dec += &c[0];
      pos += 3;
    }
    enc = pos;
  }
  return dec;
}

static char toa(int nib)
{
  if (nib < 10)
    return '0' + nib;
  return 'A' + (nib - 10);
}

std::string percent_encode(const std::string &plain)
{
  std::string encoded;
  for (auto c : plain)
  {
    char buf[4];
    if (((c >= 'A') && (c <= 'Z')) || ((c >= 'a') && (c <= 'z')) || ((c >= '0') && (c <= '9')) || (c == '-') || (c == '_') || (c == '.') || (c == '~'))
    {
      buf[0] = c;
      buf[1] = 0;
    }
    else
    {
      buf[0] = '%';
      buf[1] = toa((c >> 4) & 15);
      buf[2] = toa(c & 15);
      buf[3] = 0;
    }
    encoded += &buf[0];
  }
  return encoded;
}
