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

#include <stdlib.h>
#include <aws/core/Aws.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/core/auth/AWSCredentialsProviderChain.h>
#include <aws/core/config/AWSProfileConfigLoader.h>
#include <aws/core/client/DefaultRetryStrategy.h>
#include "nlohmann/json.hpp"
#include <fstream>
#include "log.h"
#include "resin.h"

using namespace nlohmann;

ec2_info::ec2_info(const char *filename)
{
  Aws::String region;
  auto rgn = getenv("AWS_DEFAULT_REGION");
  if (rgn)
    region = rgn;

  if (filename != 0)
  {
    if (region.size() == 0)
    {
      auto profile = (const char *)getenv("AWS_PROFILE");
      if (!profile)
        profile = "default";

      auto pfn = Aws::Auth::ProfileConfigFileAWSCredentialsProvider::GetCredentialsProfileFilename();
      Aws::Config::AWSConfigFileProfileConfigLoader loader(pfn);
      if (loader.Load())
      {
        auto profiles = loader.GetProfiles();
        auto prof = profiles.find(profile);
        if (prof != profiles.end())
          region = prof->second.GetRegion();
      }
    }
    if (region.size() == 0)
      region = "us-east-1";

    default_region = region;
    json js = json::parse(std::ifstream(filename));
    instance_id = js["instance_id"];
    ami_id = js["ami_id"];
    host_name = js["host_name"];
    private_ipv4 = js["private_ipv4"];
    public_ipv4 = js["public_ipv4"];
    user_data_js = js["user_data"];
    user_data = user_data_js.dump();
  }
  else
  {
    Aws::Client::ClientConfiguration config;
    config.connectTimeoutMs = 100;
    config.httpRequestTimeoutMs = 100;
    config.retryStrategy = std::make_shared<Aws::Client::DefaultRetryStrategy>(2, 10);
    Aws::Internal::EC2MetadataClient client(config);

    if (region.size() == 0)
      region = client.GetCurrentRegion();
    if (region.size() == 0)
      region = "us-east-1";

    default_region = region;

    ami_id = client.GetResource("/latest/meta-data/ami-id").c_str();
    if (ami_id.size() != 0)
    {
      instance_id = client.GetResource("/latest/meta-data/instance-id").c_str();
      host_name = client.GetResource("/latest/meta-data/local-hostname").c_str();
      private_ipv4 = client.GetResource("/latest/meta-data/local-ipv4").c_str();
      public_ipv4 = client.GetResource("/latest/meta-data/public-ipv4").c_str();
      user_data = client.GetResource("/latest/user-data/").c_str();
      if (user_data.size() != 0)
        user_data_js = json::parse(user_data);
    }
  }

  auto cwd = getcwd(0, 0);
  root_dir = cwd;
  root_dir += "/root";
  free(cwd);
}

namespace
{
Aws::SDKOptions options;

bool in_ec2(ec2_info &r)
{
  return r.ami_id.size() != 0 && r.instance_id.size() != 0 && r.host_name.size() != 0 && r.private_ipv4.size() != 0;
}

bool has_user_data(ec2_info &r)
{
  return r.user_data.size() != 0;
}

} // namespace

extern "C" int main(int argc, char **argv)
{
  anon_log("resin starting");
  int ret = 0;

  Aws::InitAPI(options);
  try
  {
    const char* filename = argc >= 2 ? argv[1] : (const char*)0;
    ec2_info ec2i(filename);
    if (!in_ec2(ec2i))
      anon_log("resin run outside of ec2, stopping now");
    else if (!has_user_data(ec2i))
      anon_log("resin run without supplying user data, stopping now");
    else
    {
      // we always access ec2 at the endpoint that matches the region we are in
      Aws::Client::ClientConfiguration ec2_config;
      ec2_config.region = ec2i.default_region;
      ec2i.ec2_client = std::make_shared<Aws::EC2::EC2Client>(ec2_config);

      std::string server_type = ec2i.user_data_js["server_type"];
      if (server_type == "bash_worker")
        run_worker(ec2i);
      else if (server_type == "teflon_server")
        run_server(ec2i);
      else
      {
        ret = 1;
        anon_log("unknown server_type: \"" << server_type << "\", stopping now");
      }
    }
  }
  catch (const std::exception &exc)
  {
    anon_log("resin threw uncaught exception, aborting now, what(): " << exc.what());
    ret = 1;
  }
  catch (...)
  {
    anon_log("resin threw uncaught, unknown exception, aborting now");
    ret = 1;
  }
  Aws::ShutdownAPI(options);
  return ret;
}
