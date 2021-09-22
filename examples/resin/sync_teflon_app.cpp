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
#include "nlohmann/json.hpp"
#include <sys/stat.h>
#include <aws/core/Aws.h>
#include <aws/core/utils/Outcome.h>
#include <aws/dynamodb/DynamoDBClient.h>
#include <aws/dynamodb/model/QueryRequest.h>
#include <aws/dynamodb/model/GetItemRequest.h>

using namespace nlohmann;
using namespace Aws::DynamoDB::Model;

namespace
{

struct tef_app
{
  tef_app(const Aws::String &id,
          const Aws::Vector<Aws::String> &filesv,
          Aws::Map<Aws::String, const std::shared_ptr<AttributeValue>> &fids)
      : id(id)
  {
    for (auto &f : filesv)
    {
      files[fids[f]->GetS()] = f;
    }
  }

  Aws::String id;
  std::map<Aws::String, Aws::String> files;
};

void exe_cmd(const std::string &str)
{
  std::string cmd = "bash -c '" + str + "'";
  auto f = popen(cmd.c_str(), "r");
  if (f)
  {
    auto exit_code = pclose(f);
    if (exit_code != 0)
      anon_throw(std::runtime_error, "command: " << str << " exited non-zero: " << error_string(exit_code));
  }
  else
  {
    anon_throw(std::runtime_error, "popen failed: " << errno_string());
  }
}

void create_empty_directory(const ec2_info &ec2i, const Aws::String &id)
{
  auto dir = ec2i.root_dir;
  if (id.size() != 0)
  {
    dir += "/";
    dir += id.c_str();
  }
  std::ostringstream oss;
  oss << "rm -rf " << dir;
  exe_cmd(oss.str());
  oss.str("");
  oss << "mkdir " << dir;
  exe_cmd(oss.str());
}

void remove_directory(const ec2_info &ec2i, const Aws::String &id)
{
  auto dir = ec2i.root_dir;
  if (id.size() != 0)
  {
    dir += "/";
    dir += id.c_str();
  }
  std::ostringstream oss;
  oss << "rm -rf " << id;
  exe_cmd(oss.str());
}

bool validate_user_data_string(const json& ud, const char* key)
{
  if (ud.find(key) == ud.end() || !ud[key].is_string()) {
    anon_log_error("no " << key << " in user data");
    return false;
  }
  return true;
}

bool validate_all_user_data_strings(const json& ud)
{
  return validate_user_data_string(ud, "artifacts_ddb_table_region")
    && validate_user_data_string(ud, "artifacts_ddb_table_name")
    && validate_user_data_string(ud, "artifacts_ddb_table_primary_key_name")
    && validate_user_data_string(ud, "artifacts_ddb_table_primary_key_value")
    && validate_user_data_string(ud, "artifacts_ddb_table_secondary_key_name")
    && validate_user_data_string(ud, "artifacts_ddb_table_secondary_key_value")
    && validate_user_data_string(ud, "artifacts_s3_bucket")
    && validate_user_data_string(ud, "artifacts_s3_key");
}

std::shared_ptr<tef_app> curr_app;

} // namespace

teflon_state sync_teflon_app(const ec2_info &ec2i)
{
  auto &ud = ec2i.user_data_js;
  if (!validate_all_user_data_strings(ud))
    return teflon_server_failed;

  Aws::Client::ClientConfiguration ddb_config;
  std::string region = ud["artifacts_ddb_table_region"];
  ddb_config.region = region.c_str();
  Aws::DynamoDB::DynamoDBClient ddbc(ddb_config);

  if (!curr_app)
    create_empty_directory(ec2i, "");

  std::string table_name = ud["artifacts_ddb_table_name"];
  std::string p_key_name = ud["artifacts_ddb_table_primary_key_name"];
  std::string p_key_value = ud["artifacts_ddb_table_primary_key_value"];
  std::string s_key_name = ud["artifacts_ddb_table_secondary_key_name"];
  std::string s_key_value = ud["artifacts_ddb_table_secondary_key_value"];

  QueryRequest  q_req;
  q_req.WithTableName(table_name)
    .WithKeyConditionExpression("#P = :p AND #S = :s")
    .WithScanIndexForward(false)
    .AddExpressionAttributeNames("#P", p_key_name)
    .AddExpressionAttributeValues(":p", AttributeValue(p_key_value))
    .AddExpressionAttributeNames("#S", s_key_name)
    .AddExpressionAttributeValues(":s", AttributeValue(s_key_value));

  auto outcome = ddbc.Query(q_req);
  if (!outcome.IsSuccess())
    anon_throw(std::runtime_error, "Query failed: " << outcome.GetError());
  auto &result = outcome.GetResult();
  auto &items = result.GetItems();
  if (items.size() == 0)
    anon_throw(std::runtime_error, "no item for " << p_key_value << "/" << s_key_value << " in ddb table " << table_name);
  auto &cur_def = items[0];
  auto end = cur_def.end();

  auto uid_it = cur_def.find("uid");
  if (uid_it == end)
    anon_throw(std::runtime_error, "item missing \"uid\" entry");

  auto uid = uid_it->second.GetS();
  if (curr_app && uid == curr_app->id)
  {
    anon_log("current server definition matches running server, no-op");
    return teflon_server_still_running;
  }

  auto files_it = cur_def.find("files");
  auto exe_it = cur_def.find("entry");
  auto ids_it = cur_def.find("fids");
  if (files_it == end || exe_it == end || ids_it == end)
    anon_throw(std::runtime_error, "current_server record missing required \"files\", \"entry\", and/or \"fids\" field(s)");

  auto files_needed = files_it->second.GetSS();
  auto file_to_execute = exe_it->second.GetS();
  auto ids = ids_it->second.GetM();

  std::string bucket = ud["artifacts_s3_bucket"];
  std::string key = ud["artifacts_s3_key"];

  std::ostringstream files_cmd;
  for (const auto &f : files_needed)
  {
    if (ids.find(f) == ids.end())
      anon_throw(std::runtime_error, "file: \"" << f << "\" missing in fids");
    if (curr_app && curr_app->files.find(f) != curr_app->files.end())
    {
      // f matches a file we have already downloaded, so just hard link it
      // to the new directory
      auto existing_file = curr_app->files[f];
      files_cmd << "ln " << ec2i.root_dir << "/" << curr_app->id << "/" << curr_app->files[f]
                << " " << ec2i.root_dir << "/" << uid << "/" << f << " || exit 1 &\n";
    }
    else
    {
      // does not match an existing file, so download it from s3
      files_cmd << "aws s3 cp s3://" << bucket << "/" << key
                << "/" << ids[f]->GetS() << "/" << f << " "
                << ec2i.root_dir << "/" << uid << "/" << f << " --quiet || exit 1 &\n";
    }
  }
  create_empty_directory(ec2i, uid);

  files_cmd << "wait < <(jobs -p)\n";
  anon_log("copying files with: " << files_cmd.str());

  // we have to be careful of ec2 permissions behavior during early
  // instance startup.  I suspect there is additional strange behavior
  // of the aws command line tools during that period.  If the file
  // copy fails to copy any files, then we sleep a really long time
  // and try again.  We repeat this for the first 5 minute of bootup.
  // If it is still failing after 8 min we give up.
  auto num_tries = 0;
  while (true) {
    try {
      exe_cmd(files_cmd.str());
      if (files_needed.size() > 0) {
        std::ostringstream oss;
        oss << "stat " << ec2i.root_dir << "/" << uid << "/" << files_needed[0];
        exe_cmd(oss.str());
      }
      break;
    }
    catch(...) {
      anon_log_error("file copy failed");
      if (++num_tries > 10) {
        remove_directory(ec2i, uid);
        return teflon_server_failed;
      }
      sleep(45);
    }
  }

  std::ostringstream ef;
  ef << ec2i.root_dir << "/" << uid << "/" << file_to_execute;
  auto efs = ef.str();
  chmod(efs.c_str(), ACCESSPERMS);

  auto do_tls = false;
  std::vector<std::string> args;

  if (ud.find("certs_ddb_table_name") != ud.end()
      && ud.find("serving_domain") != ud.end()) {

    Aws::Client::ClientConfiguration home_ddb_config;
    std::string home_region = ud["home_region"];
    home_ddb_config.region = home_region.c_str();
    Aws::DynamoDB::DynamoDBClient home_ddbc(home_ddb_config);

    std::string domain = ud["serving_domain"];
    std::string table = ud["certs_ddb_table_name"];
    GetItemRequest  req;
    req.WithTableName(table)
      .AddKey("domain", AttributeValue(domain));
    auto outcome = home_ddbc.GetItem(req);
    if (!outcome.IsSuccess())
      anon_throw(std::runtime_error, "DynamoDB.GetItem(" << table << "/" << domain << ") failed: " << outcome.GetError().GetMessage());

    auto &item = outcome.GetResult().GetItem();
    auto fc_it = item.find("fullchain");
    auto key_it = item.find("privkey");
    if (fc_it == item.end() || key_it == item.end())
      anon_throw(std::runtime_error, "dynamodb entry missing required fullchain and/or privkey entries");

    auto certs_file = ec2i.root_dir + "/fullchain.pem";
    std::ostringstream certs_cmd;
    certs_cmd << "echo \"" << fc_it->second.GetS() << "\" >> " << certs_file;
    exe_cmd(certs_cmd.str());

    auto key_file = ec2i.root_dir + "/privkey.pem";
    std::ostringstream key_cmd;
    key_cmd << "echo \"" << key_it->second.GetS() << "\" >> " << key_file;
    exe_cmd(key_cmd.str());

    args.push_back("-cert_verify_dir");
    args.push_back("/etc/ssl/certs");
    args.push_back("-server_cert");
    args.push_back(certs_file);
    args.push_back("-server_key");
    args.push_back(key_file);
    do_tls = true;
  }


  try
  {
    auto new_app = std::make_shared<tef_app>(uid, files_needed, ids);
    start_server(efs.c_str(), do_tls, args);
    if (curr_app)
      remove_directory(ec2i, curr_app->id);
    curr_app = new_app;
  }
  catch (const std::exception &exc)
  {
    anon_log_error("start_server failed: " << exc.what());
    remove_directory(ec2i, uid);
    return teflon_server_failed;
  }
  catch (...)
  {
    anon_log_error("start_server");
    remove_directory(ec2i, uid);
    return teflon_server_failed;

  }

  return teflon_server_running;
}
