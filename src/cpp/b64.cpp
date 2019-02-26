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

#include "b64.h"
#include "log.h"
#include <stdexcept>
#include <vector>

#if 0

From: https://tools.ietf.org/html/rfc4648

       Table 2: The "URL and Filename safe" Base 64 Alphabet

     Value Encoding  Value Encoding  Value Encoding  Value Encoding
         0 A            17 R            34 i            51 z
         1 B            18 S            35 j            52 0
         2 C            19 T            36 k            53 1
         3 D            20 U            37 l            54 2
         4 E            21 V            38 m            55 3
         5 F            22 W            39 n            56 4
         6 G            23 X            40 o            57 5
         7 H            24 Y            41 p            58 6
         8 I            25 Z            42 q            59 7
         9 J            26 a            43 r            60 8
        10 K            27 b            44 s            61 9
        11 L            28 c            45 t            62 - (minus)
        12 M            29 d            46 u            63 _ (underline)
        13 N            30 e            47 v           
        14 O            31 f            48 w
        15 P            32 g            49 x
        16 Q            33 h            50 y         (pad) =

#endif

static const char alphabet[64] = {
    'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M',
    'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
    'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm',
    'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z',
    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '-', '_'};

std::string b64url_encode(const char *data, size_t len, char pad)
{
  // each complete block of 3 bytes of data will encode to 4 bytes of result
  // if the last block of 3 bytes is incomplete (that is, len % 3 != 0)
  // then the last characters are padded with 'pad' (by default '=')
  // or left out of the encoded string if 'pad' is 0.  When the padding is left
  // out each source byte in the incomplete causes one byte in the encoded data
  int num_full_blocks = len / 3;
  auto lm3 = (len % 3);
  int encoded_size = pad ? ((len + 2) / 3 * 4) : (num_full_blocks * 4 + (lm3 != 0 ? lm3 + 1 : 0));

  std::vector<char> result(encoded_size);

  const unsigned char *ptr = (const unsigned char *)data;
  const unsigned char *end = ptr + len;
  int i = 0;

  for (; i < num_full_blocks; i++, ptr += 3)
  {
    result[i * 4 + 0] = alphabet[ptr[0] >> 2];
    result[i * 4 + 1] = alphabet[((ptr[0] << 4) + (ptr[1] >> 4)) & 0x3f];
    result[i * 4 + 2] = alphabet[((ptr[1] << 2) + (ptr[2] >> 6)) & 0x3f];
    result[i * 4 + 3] = alphabet[ptr[2] & 0x3f];
  }

  if (ptr < end)
  {
    result[i * 4 + 0] = alphabet[ptr[0] >> 2];
    if (&ptr[1] < end)
    {
      result[i * 4 + 1] = alphabet[((ptr[0] << 4) + (ptr[1] >> 4)) & 0x3f];
      result[i * 4 + 2] = alphabet[(ptr[1] << 2) & 0x3f];
      if (pad)
        result[i * 4 + 3] = pad;
    }
    else
    {
      result[i * 4 + 1] = alphabet[(ptr[0] << 4) & 0x3f];
      if (pad)
      {
        result[i * 4 + 2] = pad;
        result[i * 4 + 3] = pad;
      }
    }
  }

  return std::string(&result[0], encoded_size);
}

static unsigned char b64_index(unsigned char code)
{
  if (code >= 'A' && code <= 'Z')
    return code - 'A';
  if (code >= 'a' && code <= 'z')
    return code - 'a' + 26;
  if (code >= '0' && code <= '9')
    return code - '0' + 52;
  if (code == '-')
    return 62;
  if (code == '_')
    return 63;
  char str[2];
  str[0] = code;
  str[1] = 0;
  anon_throw(std::runtime_error, "invalid character in b64 string: (" << (int)code << ") \"" << &str[0] << "\"");
}

std::string b64url_decode(const char *data, size_t len, char pad)
{
  if (pad)
  {
    if (len % 4)
    {
      anon_throw(std::runtime_error, "illegal base64 length - must be a multiple of 4, was: " << len);
    }
  }
  else
  {
    if ((len % 4) == 1)
    {
      anon_throw(std::runtime_error, "illegal unpadded base64 length - (len % 4) cannot == 1, len: " << len);
    }
  }
  int num_pads = 0;
  if (pad)
  {
    if (len > 0)
    {
      if (data[len - 2] == pad)
        num_pads = 2;
      else if (data[len - 1] == pad)
        num_pads = 1;
    }
  }

  len -= num_pads;
  int num_full_blocks = len / 4;
  int decoded_size = (num_full_blocks * 3) + (len % 4);

  std::string result(decoded_size, 0);

  const unsigned char *ptr = (const unsigned char *)data;
  const unsigned char *end = ptr + len;
  int i = 0;

  for (; i < num_full_blocks; i++, ptr += 4)
  {
    auto a = b64_index(ptr[0]);
    auto b = b64_index(ptr[1]);
    auto c = b64_index(ptr[2]);
    auto d = b64_index(ptr[3]);

    result[i * 3 + 0] = (a << 2) + (b >> 4);
    result[i * 3 + 1] = (b << 4) + (c >> 2);
    result[i * 3 + 2] = (c << 6) + d;
  }

  if (ptr < end)
  {
    auto a = b64_index(ptr[0]);
    auto b = b64_index(ptr[1]);
    result[i * 3 + 0] = (a << 2) + (b >> 4);
    if (&ptr[1] < end)
    {
      auto c = b64_index(ptr[2]);
      result[i * 3 + 1] = (b << 4) + (c >> 2);
    }
  }

  return result;
}
