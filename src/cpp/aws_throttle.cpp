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

#ifdef ANON_AWS

#include "aws_throttle.h"
#include "fiber.h"
#include "time_utils.h"
#include <map>

namespace {
std::map<std::string, timespec> wait_until;
fiber_mutex mtx;
}

void aws_throttle(const std::string& region, const std::function<void()>& fn)
{
  const auto backoff_seconds = 5;
  const auto max_backoff_attempts = 100;
  auto backoff_attempts = 0;
  while (true)
  {
    try {
      {
        fiber_lock l(mtx);
        double sleep_seconds;
        while ((sleep_seconds = to_seconds(wait_until[region] - cur_time())) > 0)
        {
          l.unlock();
          fiber::msleep((int)(sleep_seconds * 1000));
          l.lock();
        }
      }
      fn();
      break;
    }
    catch(const aws_throttle_error& e)
    {
      if (++backoff_attempts > max_backoff_attempts)
        throw e;
      fiber_lock l(mtx);
      wait_until[region] = cur_time() + backoff_seconds;
      anon_log("throttling error caught, for " << region << ", setting backoff to " << backoff_seconds << " more seconds");
    }
  }
}

#endif
