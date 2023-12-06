/*
 Copyright (c) 2023 Anon authors, see AUTHORS file.
 
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

#include "sctp_dispatch.h"
#include "fiber.h"

namespace {
void append_nibble(uint8_t nib, std::ostringstream& oss, char a = 'a')
{
  char b[2] = {0};
  if (nib < 10) {
    b[0] = '0' + nib;
  }
  else {
    b[0] = a + (nib - 10);
  }
  oss << &b[0];
}

// debugging helper
void append_byte(unsigned char b, bool add_comma, std::ostringstream& oss)
{
  oss << "0x";
  append_nibble(b >> 4, oss);
  append_nibble(b & 0x0f, oss);
  if (add_comma) {
    oss << ", ";
  }
}

// debugging helper
void append_bytes(const unsigned char* bytes, size_t len, std::ostringstream& oss)
{
  oss << "[";
  for (size_t i = 0; i < len; i++) {
    append_byte(bytes[i], i != len - 1, oss);
  }
  oss << "]";
}

}

void sctp_dispatch::recv_msg(const uint8_t *msg, ssize_t len)
{
  std::ostringstream oss;
  append_bytes(msg, len, oss);
  anon_log("got sctp msg:\n" << oss.str());
}

