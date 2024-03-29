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

#pragma once

#include <string>
#include <utility>
#include <functional>

// execute the given command (in 'sh' - using popen) and return:
//  1) exit code of command in .first
//  2) the first line of the command's stdout in .second
std::string exe_cmd_(const std::function<void(std::ostream &formatter)>& fn, bool first_line_only);

#define exe_cmd1(_body) exe_cmd_([&](std::ostream &formatter) { formatter << _body; }, true)
#define exe_cmd(_body) exe_cmd_([&](std::ostream &formatter) { formatter << _body; }, false)

void reset_exe_cmd_count();
int get_exe_cmd_count();
