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

#include "sync_teflon_app.h"
#include "log.h"
#include "sproc_mgr.h"
#include <sys/stat.h>
#include <aws/core/Aws.h>
#include <aws/core/utils/Outcome.h>
#include <aws/dynamodb/DynamoDBClient.h>
#include <aws/dynamodb/model/GetItemRequest.h>

namespace
{

struct tef_app
{
  tef_app(const Aws::String& id,
    const Aws::Vector<Aws::String>& filesv,
    Aws::Map<Aws::String, const std::shared_ptr<Aws::DynamoDB::Model::AttributeValue>>& fids)
    : id(id)
  {
    for (auto &f : filesv) {
      files[fids[f]->GetS()] = f;
    }
  }

  Aws::String id;
  std::map<Aws::String, Aws::String> files;
};

bool exe_cmd(const std::string &str)
{
  std::string cmd = "bash -c '" + str + "'";
  auto f = popen(cmd.c_str(), "r");
  if (f)
  {
    auto exit_code = pclose(f);
    if (exit_code != 0)
    {
      anon_log_error("command: " << str << " exited non-zero: " << error_string(exit_code));
      return false;
    }
  }
  else
  {
    anon_log_error("popen failed: " << errno_string());
    return false;
  }
  return true;
}

bool create_empty_directory(const ec2_info &ec2i, const Aws::String &id)
{
  auto dir = ec2i.root_dir;
  if (id.size() != 0)
  {
    dir += "/";
    dir += id.c_str();
  }
  std::ostringstream str;
  str << "rm -rf " << dir << " && mkdir " << dir;
  return exe_cmd(str.str());
}

void remove_directory(const ec2_info &ec2i, const Aws::String &id)
{
  auto dir = ec2i.root_dir;
  if (id.size() != 0)
  {
    dir += "/";
    dir += id.c_str();
  }
  std::ostringstream str;
  str << "rm -rf " << id;
  if (!exe_cmd(str.str()))
    anon_log_error("failed to remove " << id << " directory");
}

std::shared_ptr<tef_app> curr_app;

} // namespace

teflon_state sync_teflon_app(const ec2_info &ec2i)
{
  if (ec2i.user_data_js.find("current_server_primary_key_value") == ec2i.user_data_js.end() || ec2i.user_data_js.find("current_server_primary_key_name") == ec2i.user_data_js.end())
  {
    anon_log_error("no current server info specified in user data - cannot start teflon app");
    return teflon_server_failed;
  }

  if (!curr_app)
  {
    if (!create_empty_directory(ec2i, ""))
      return teflon_server_failed;
  }

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
  if (!outcome.IsSuccess())
  {
    anon_log_error("GetItem failed: " << outcome.GetError());
    return teflon_server_failed;
  }

  auto &map = outcome.GetResult().GetItem();
  auto state_it = map.find("state");
  if (state_it == map.end())
  {
    anon_log_error("current_server record missing required \"state\" field");
    return teflon_server_failed;
  }
  auto state = state_it->second.GetS();
  if (state == "stop")
  {
    anon_log("stopping server");
    return teflon_shut_down;
  }

  if (state != "run")
  {
    anon_log_error("unknown teflon server state: " << state);
    return teflon_server_failed;
  }

  auto id_it = map.find("current_server_id");

  auto end = map.end();
  if (id_it == end)
  {
    anon_log_error("current_server record missing required \"current_server_id\" field");
    return teflon_server_failed;
  }

  auto current_server_id = id_it->second.GetS();
  if (curr_app && current_server_id == curr_app->id)
  {
    anon_log("current server definition matches running server, no-op");
    return teflon_server_running;
  }

  auto files_it = map.find("files");
  auto exe_it = map.find("entry");
  auto ids_it = map.find("fids");
  if (files_it == end || exe_it == end || ids_it == end)
  {
    anon_log_error("current_server record missing required \"files\", \"entry\", and/or \"fids\" field(s)");
    return teflon_server_failed;
  }

  auto files_needed = files_it->second.GetSS();
  auto file_to_execute = exe_it->second.GetS();
  auto ids = ids_it->second.GetM();

  if (ec2i.user_data_js.find("current_server_artifacts_bucket") == ec2i.user_data_js.end() || ec2i.user_data_js.find("current_server_artifacts_key") == ec2i.user_data_js.end())
  {
    anon_log_error("user data missing required fields \"current_server_artifacts_bucket\" and/or \"current_server_artifacts_key\"");
    return teflon_server_failed;
  }
  std::string bucket = ec2i.user_data_js["current_server_artifacts_bucket"];
  std::string key = ec2i.user_data_js["current_server_artifacts_key"];

  std::ostringstream files_cmd;
  for (const auto &f : files_needed)
  {
    if (ids.find(f) == ids.end())
    {
      anon_log_error("file: \"" << f << "\" missing in fids");
      return teflon_server_failed;
    }
    if (curr_app && curr_app->files.find(f) != curr_app->files.end())
    {
      // f matches a file we have already downloaded, so just hard link it
      // to the new directory
      auto existing_file = curr_app->files[f];
      files_cmd << "ln " << ec2i.root_dir << "/" << curr_app->id << "/" << curr_app->files[f]
               << " " << ec2i.root_dir << "/" << current_server_id << "/" << f << " || exit 1 &\n";
    }
    else
    {
      // does not match an existing file, so download it from s3
      files_cmd << "aws s3 --region " << ec2i.default_region << " cp s3://" << bucket << "/" << key << "/" << f
               << " " << ec2i.root_dir << "/" << current_server_id << "/" << f << " --quiet || exit 1 &\n";
    }
  }
  if (!create_empty_directory(ec2i, current_server_id))
    return teflon_server_failed;
  files_cmd << "wait < <(jobs -p)\n";
  if (!exe_cmd(files_cmd.str()))
    return teflon_server_failed;

  std::ostringstream ef;
  ef << ec2i.root_dir << "/" << current_server_id << "/" << file_to_execute;
  auto efs = ef.str();
  chmod(efs.c_str(), ACCESSPERMS);

  try {
    auto new_app = std::make_shared<tef_app>(current_server_id, files_needed, ids);
    start_server(efs.c_str(), false/*do_tls*/, std::vector<std::string>());
    if (curr_app)
      remove_directory(ec2i, curr_app->id);
    curr_app = new_app;
  }
  catch(const std::exception& exc)
  {
    anon_log_error("start_server failed: " << exc.what());
    remove_directory(ec2i, current_server_id);
    return teflon_server_failed;
  }
  catch(...)
  {
    anon_log_error("start_server");
    remove_directory(ec2i, current_server_id);
    return teflon_server_failed;
  }

  return teflon_server_running;
}
