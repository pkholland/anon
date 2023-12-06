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

#pragma once

#include <functional>
#include <string>
#include <vector>
#include <memory>

class stun_msg_parser
{
  std::function<std::shared_ptr<std::string>(const std::string& username)> lookup;

public:

  struct stun_msg {
    uint16_t method;
    uint16_t method_class;
    std::string remote_ufrag;
    std::string remote_pwd;
    std::string local_ufrag;
    std::string local_pwd;
    std::string user_name;
    bool valid;
    bool has_fingerprint;
    bool has_use_candidate;
    bool has_ice_controlling;
    bool known_client;

    stun_msg(
        uint16_t method,
        uint16_t method_class,
        const std::string& remote_ufrag,
        const std::string& remote_pwd,
        const std::string& local_ufrag,
        const std::string& local_pwd,
        bool has_fingerprint,
        bool has_use_candidate,
        bool has_ice_controlling,
        bool known_client)
      : method(method),
        method_class(method_class),
        remote_ufrag(remote_ufrag),
        remote_pwd(remote_pwd),
        local_ufrag(local_ufrag),
        local_pwd(local_pwd),
        valid(true),
        has_fingerprint(has_fingerprint),
        has_use_candidate(has_use_candidate),
        has_ice_controlling(has_ice_controlling),
        known_client(known_client)
    {}

    stun_msg()
      : valid(false)
    {}

    stun_msg(stun_msg&& rhs) = default;

    operator bool() const { return valid; }
  };

  stun_msg_parser(std::function<std::shared_ptr<std::string>(const std::string& username)> lookup);

  stun_msg parse_stun_msg(const uint8_t *msg, ssize_t len);
  std::vector<uint8_t> create_stun_reply(stun_msg&& stun, const uint8_t* msg,
    const struct sockaddr_storage *sockaddr);

};
