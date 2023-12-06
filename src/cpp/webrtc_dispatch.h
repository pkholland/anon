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

#include "udp_dispatch.h"
#include "nlohmann/json.hpp"
#include "tls_context.h"
#include "stun.h"
#include "dtls_dispatch.h"

class webrtc_dispatch : public udp_dispatch
{
  std::function<void(const std::string& key, const std::shared_ptr<std::string>& val)> store_resource;
  std::shared_ptr<tls_context> dtls_context;
  std::string x509_fingerprint_attribute;
  std::string serving_ip_addr;
  stun_msg_parser stun;
  std::shared_ptr<dtls_dispatch> dtls;

  void recv_msg(
    const unsigned char *msg,
    ssize_t len,
    const struct sockaddr_storage *sockaddr,
    socklen_t sockaddr_len) override;

public:
  // brief: construct an object that reads from and writes to the socket
  // 'udp_socket' - which is expected to be a UDP socket.  This object
  // implements the webrtc protocol on that socket.
  webrtc_dispatch(int udp_socket,
    const std::string& cert_file_name,
    const std::string& priv_key_file_name,
    const std::string& serving_ip_addr,
    std::function<std::shared_ptr<std::string>(const std::string& username)> read_resource,
    std::function<void(const std::string& key, const std::shared_ptr<std::string>& val)> store_resource);

  // given an sdp "offer" this attempts to parse it as such and returns an "answer"
  // that should be given to whatever client passed this offer.  This is the "signaling"
  // step in webrtc.  The expectation is that there is some normal service endpoint that
  // accepts these offers.  It's probably being called by some javascript that is doing
  // "const pc = new RTCConnection()" logic.  This assumes that the endpoint has already
  // accepted the credentials of whoever called this signaling endpoint, and that the
  // intent in calling this parse_offer function is to accept that offer and allow the
  // original caller to make webrtc connections.
  nlohmann::json parse_offer(const nlohmann::json& offer);
};
