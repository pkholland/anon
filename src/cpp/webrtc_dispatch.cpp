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

#include "webrtc_dispatch.h"
#include "webrtc_connection.pb.h"
#include "http_error.h"
#include "big_id_serial.h"
#include "big_id_crypto.h"

using namespace nlohmann;

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

void append_sdp_byte(uint8_t b, std::ostringstream& oss, bool include_separator)
{
  append_nibble(b >> 4, oss, 'A');
  append_nibble(b & 0x0f, oss, 'A');
  if (include_separator) {
    oss << ":";
  }
}

std::pair<bool, char> get_nibble(char c)
{
  if (c >= 'A' && c <= 'F') {
    return std::make_pair(true, c - 'A' + 10);
  }
  if (c >= '0' && c <= '9') {
    return std::make_pair(true, c - '0');
  }
  return std::make_pair(false, 'a');
}

std::string digest_from_fingerprint_str(const char* fgp, size_t len)
{
  std::ostringstream oss;
  auto ptr = fgp;
  auto end = fgp+len;
  while (ptr < end-2) {
    if (ptr < end-3 && ptr[2] != ':') {
      return {};
    }
    auto n1 = get_nibble(ptr[0]);
    auto n2 = get_nibble(ptr[1]);
    if (!n1.first || !n2.first) {
      return {};
    }
    auto c = (char)((n1.second << 4) + n2.second);
    oss << std::string(&c, 1);
    ptr += 3;
  }
  return oss.str();
}

}

webrtc_dispatch::webrtc_dispatch(int udp_socket,
    const std::string& cert_file_name,
    const std::string& priv_key_file_name,
    const std::string& serving_ip_addr,
    std::function<std::shared_ptr<std::string>(const std::string& username)> read_resource,
    std::function<void(const std::string& key, const std::shared_ptr<std::string>& val)> store_resource)
  : udp_dispatch(udp_socket, true, true),
    serving_ip_addr(serving_ip_addr),
    stun(std::move(read_resource)),
    store_resource(std::move(store_resource)),
    dtls_context(std::make_shared<tls_context>(false, // for the server, not client
                cert_file_name.c_str(),
                priv_key_file_name.c_str(),
                5))
{
  auto digest = dtls_context->sha256_digest();
  std::ostringstream oss;
  oss << "a=fingerprint:sha-256 ";
  for (auto i = 0; i < digest.size(); i++) {
    append_sdp_byte((uint8_t)digest[i], oss, i < digest.size() - 1);
  }
  oss << "\r\n";
  x509_fingerprint_attribute = oss.str();

  dtls = std::make_shared<dtls_dispatch>(dtls_context, udp_socket);
}

json webrtc_dispatch::parse_offer(const nlohmann::json& offer)
{
  // RFC 4145 for an explanation of the attributes in an sdp string
  
  std::string type_str = "type";
  std::string offer_str = "offer";
  std::string answer_str = "answer";
  std::string sdp_str = "sdp";

  auto type_it = offer.find(type_str);
  if (type_it == offer.end() || !type_it->is_string() || offer_str != *type_it) {
    anon_log("request contains no \"type\" or it isn't a string, or its value isn't \"offer\"");
    throw_request_error(HTTP_STATUS_BAD_REQUEST, "invalid offer");
  }
  auto sdp_it = offer.find(sdp_str);
  if (sdp_it == offer.end() || !sdp_it->is_string()) {
    anon_log("offer contains no \"sdp\" or it isn't a string");
    throw_request_error(HTTP_STATUS_BAD_REQUEST, "invalid offer");
  }
  std::string sdp = *sdp_it;

  std::ostringstream ret_sdp;
  std::string remote_ufrag;
  std::string remote_pwd;
  std::string remote_x509_digest;
  int32_t remote_sctp_port{0};
  std::string local_ufrag;
  std::string local_pwd;
  int32_t local_sctp_port{5000};
  auto line_start = &sdp.c_str()[0];
  auto body_end = &sdp.c_str()[sdp.size()];
  while (line_start < body_end) {
    auto line_end = std::strchr(line_start, '\r');
    if (!line_end || line_end == body_end-1 || line_end[1] != '\n') {
      anon_log("invalid sdp in offer");
      throw_request_error(HTTP_STATUS_BAD_REQUEST, "invalid sdp in offer");
    }
    if (line_end > line_start + 1 && line_start[1] == '=') {
      switch(*line_start) {
        case 'a':
          if (!memcmp("a=ice-pwd:", line_start, 10)) {
            remote_pwd = std::string(line_start+10,line_end-line_start-10);
            local_pwd = toHexString(small_rand_id());
            ret_sdp << "a=ice-pwd:" << local_pwd + "\r\n";
          }
          else if (!memcmp("a=ice-ufrag:", line_start, 12)) {
            remote_ufrag = std::string(line_start+12,line_end-line_start-12);
            local_ufrag = toHexString(small_rand_id());
            ret_sdp << "a=ice-ufrag:" << local_ufrag << "\r\n";
          }
          else if (!memcmp("a=setup:", line_start, 8)) {
            ret_sdp << "a=setup:passive\r\n";
          }
          else if (!memcmp("a=sctp-port:", line_start, 12)) {
            remote_sctp_port = std::atoi(&line_start[2]);
            ret_sdp << "a=sctp-port:" << local_sctp_port << "\r\n";
          }
          else if (!memcmp("a=fingerprint:sha-256 ", line_start, 22)) {
            remote_x509_digest = digest_from_fingerprint_str(&line_start[22], line_end-line_start-22);
            ret_sdp << x509_fingerprint_attribute;
          }
          else {
            ret_sdp << std::string(line_start, line_end + 2 - line_start);
          }
          break;
        default:
          ret_sdp << std::string(line_start, line_end + 2 - line_start);
          break;
      }
    }
    line_start = line_end + 2;
  }

  if (local_pwd.empty() || local_ufrag.empty() || ret_sdp.str().empty() || remote_x509_digest.empty()) {
    anon_log("no pwd or empty sdp in offer");
    throw_request_error(HTTP_STATUS_BAD_REQUEST, "invalid sdp in offer");
  }
  ret_sdp << "a=candidate:0 1 udp 2122260223 " << serving_ip_addr << " " << get_port() << " typ host\r\n";

  webrtc::Connection conn;
  *conn.mutable_remote_pwd() = remote_pwd;
  *conn.mutable_remote_ufrag() = remote_ufrag;
  conn.set_remote_sctp_port(remote_sctp_port);
  *conn.mutable_remote_x509_digest() = remote_x509_digest;
  *conn.mutable_local_pwd() = local_pwd;
  *conn.mutable_local_ufrag() = local_ufrag;
  conn.set_local_sctp_port(local_sctp_port);
  auto res = std::make_shared<std::string>(conn.SerializeAsString());
  auto username = local_ufrag + ":" + remote_ufrag;
  store_resource(username, res);

  return {{type_str, answer_str}, {sdp_str, ret_sdp.str()}};
}

void webrtc_dispatch::recv_msg(
  const unsigned char *msg,
  ssize_t len,
  const struct sockaddr_storage *addr,
  socklen_t addr_len)
{
  // this (partially) implements the multiplexing step described in RFC 7983
  if (len > 0) {
    auto first_byte = msg[0];
    if (first_byte <= 3) {
      if (auto s = stun.parse_stun_msg(msg, len)) {
        if (s.known_client) {
          dtls->register_address(addr);
          auto reply = stun.create_stun_reply(std::move(s), msg, addr);
          if (sendto(get_sock(), &reply[0], reply.size(), 0, (sockaddr*)addr, addr_len) == -1) {
            anon_log("sendto failed");
          }
        }
      }
      else {
        anon_log("failed to parse as stun");
      }
    }
    else if (first_byte >= 16 && first_byte <= 19) {
      anon_log("ignoring likely ZRTP message (" << (int)first_byte << ")");
    }
    else if (first_byte >= 20 && first_byte <= 63) {
      dtls->recv_msg(msg, len, addr);
    }
    else if (first_byte >= 64 && first_byte <= 79) {
      anon_log("ignoring likely TURN Channel message (" << (int)first_byte << ")");
    }
    else if (first_byte >= 128 && first_byte <= 191) {
      anon_log("TODO: ignoring likely RTP/RTCP message (" << (int)first_byte << ")");
    }
    else {
      anon_log("unknown first byte range: (" << (int)first_byte << ")");
    }
  }
}
