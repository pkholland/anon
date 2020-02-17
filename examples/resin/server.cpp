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
#include <aws/core/Aws.h>
#include <aws/core/utils/Outcome.h>
#include <aws/dynamodb/DynamoDBClient.h>
#include <aws/dynamodb/model/GetItemRequest.h>
#include <sys/stat.h>
#include "server_control.h"

void exe_cmd(const std::string& str)
{
  auto f = popen(str.c_str(), "r");
  if (f) {
    auto exit_code = pclose(f);
    if (exit_code != 0)
      anon_log("command: " << str << " exited non-zero: " << error_string(exit_code));
  }
  else
    anon_log("popen failed: " << errno_string());
}

std::string create_empty_directory(const std::string& id)
{
  std::ostringstream str;
  str << "rm -rf root/" << id << " && mkdir root/" << id;
  exe_cmd(str.str());
  return std::string("root/") + id;
}

std::string get_file_set(const std::string& working_dir, const ec2_info &ec2i, const Aws::Vector<Aws::String>& files, const Aws::String& exe_file, const Aws::String id)
{
  auto dir = create_empty_directory(id.c_str());
  std::string bucket = ec2i.user_data_js["current_server_artifacts_bucket"];
  std::string key = ec2i.user_data_js["current_server_artifacts_key"];

  // as of around 2019 all s3 buckets and data are global, in the sense that
  // you can hit any s3 service endpoint and ask for files and they will all
  // return the same content.  But there is still an advanage to asking for
  // the endpoint that we are running on, since that one is closer to us.

  for (auto& f : files) {
    std::ostringstream str;
    str << "aws s3 --region " << ec2i.default_region << " cp s3://" << bucket << "/" << key << "/" << f << " " << dir << "/" << f;
    exe_cmd(str.str());
  }

  std::ostringstream str;
  str << working_dir << "/" << dir << "/" << exe_file;
  chmod(str.str().c_str(), ACCESSPERMS);

  return dir + "/" + exe_file.c_str();
}

void run_server(const ec2_info &ec2i)
{
  int port = ec2i.user_data_js["server_port"];
  int cnt_port = ec2i.user_data_js["control_port"];

  std::vector<char> working_directory(2048);
  getcwd(&working_directory[0], working_directory.size());
  std::string working_dir(&working_directory[0]);
  create_empty_directory("");

  Aws::Client::ClientConfiguration ddb_config;
  if (ec2i.user_data_js.find("current_server_region") != ec2i.user_data_js.end())
    ddb_config.region = ec2i.user_data_js["current_server_region"];
  else
    ddb_config.region = ec2i.default_region;
  Aws::DynamoDB::DynamoDBClient ddbc(ddb_config);

  Aws::DynamoDB::Model::AttributeValue primary_key;
  std::string cs_primary_key = ec2i.user_data_js["current_server_primary_key_value"];
  primary_key.SetS(cs_primary_key.c_str());
  Aws::DynamoDB::Model::GetItemRequest req;
  std::string cs_table_name = ec2i.user_data_js["current_server_table_name"];
  std::string cs_key_name = ec2i.user_data_js["current_server_primary_key_name"];
  req.WithTableName(cs_table_name.c_str())
      .AddKey(cs_key_name.c_str(), primary_key);
  auto outcome = ddbc.GetItem(req);
  if (outcome.IsSuccess()) {
    anon_log("read the table entry");
    auto &map = outcome.GetResult().GetItem();
    auto dwn_it = map.find("files");
    auto exe_it = map.find("entry");
    auto id_it = map.find("current_server_id");
    if ((dwn_it != map.end()) && (exe_it != map.end()) && (id_it != map.end())) {
      auto files_to_download = dwn_it->second.GetSS();
      auto file_to_execute = exe_it->second.GetS();
      auto id = id_it->second.GetS();
      auto exe_file = working_dir + "/"
        + get_file_set(working_dir, ec2i, files_to_download, file_to_execute, id);

      sproc_mgr_init(port);
      anon_log("epoxy bound to network port " << port);

      start_server(exe_file.c_str(), false/*do_tls*/, std::vector<std::string>());
      
      run_server_control(ec2i, cnt_port);

      stop_server();

      sproc_mgr_term();

    }
  }
  else
    anon_log("failed to read the entry, error: " << outcome.GetError());





}
