/*
 Copyright (c) 2020 Anon authors, see AUTHORS file.
 
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

#include "exe_cmd.h"
#include <sstream>
#include "log.h"

std::string exe_cmd_(const std::function<void(std::ostream &formatter)>& fn, bool first_line_only)
{
  std::ostringstream cmd;
  fn(cmd);
  auto f = popen(cmd.str().c_str(), "r");
  if (f)
  {
    std::ostringstream str;
    char buff[1024];
    auto indx = 0;
    int c;
    while ((c = getc(f)) != EOF) {
      if (first_line_only && c == '\n')
        break;
      if (indx == sizeof(buff)) {
        str << std::string(&buff[0], indx);
        indx = 0;
      }
      buff[indx++] = (char)c;
    }
    str << std::string(&buff[0], indx);
    auto pc = pclose(f);
    if (pc == -1)
      anon_log_error("pclose failed");
    else if (pc > 0)
      anon_throw(std::runtime_error, "command - " << cmd.str() << " - failed: " << error_string(pc));
    return str.str();
  }
  anon_throw(std::runtime_error, "popen failed: " << errno_string());
}
