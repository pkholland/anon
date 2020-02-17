/*
 Copyright (c) 2020 ANON authors, see AUTHORS file.
 
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

#include "resin.h"
#include "sproc_mgr.h"
#include "log.h"
#include "start_teflon_app.h"
#include "server_control.h"

void run_server(const ec2_info &ec2i)
{
  if (ec2i.user_data_js.find("server_port") == ec2i.user_data_js.end()
    || ec2i.user_data_js.find("control_port") == ec2i.user_data_js.end())
  {
    anon_log_error("user data missing required \"server_port\" and/or \"control_port\"");
    return;
  }
  int port = ec2i.user_data_js["server_port"];
  int cnt_port = ec2i.user_data_js["control_port"];

  sproc_mgr_init(port);
  anon_log("epoxy bound to network port " << port);

  if (start_teflon_app(ec2i) != teflon_server_running) {
    anon_log_error("cannot start teflon app - shutting down");
    sproc_mgr_term();
    return;
  }

  run_server_control(ec2i, cnt_port);

  stop_server();

  sproc_mgr_term();

}
