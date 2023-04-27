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
#include <aws/core/utils/logging/ConsoleLogSystem.h>
#include <aws/crt/ImdsClient.h>
#include <aws/core/Globals.h>
#include "nlohmann/json.hpp"
#include <fstream>
#include "log.h"
#include "resin.h"

using namespace nlohmann;

struct helper {
  ec2_info* ec2_inf;
  std::mutex  mtx;
  std::condition_variable cond;
  int items;
};

void dec(void* userData)
{
  auto h = (helper*)userData;
  if (--h->items == 0)
    h->cond.notify_all();
}

ec2_info::ec2_info(const char *filename)
{
  Aws::Crt::Imds::ImdsClientConfig imdsConfig;
  imdsConfig.Bootstrap = Aws::GetDefaultClientBootstrap();
  Aws::Crt::Imds::ImdsClient imdsClient(imdsConfig);

  instance_id = "instance_id";
  ami_id = "ami_id";
  private_ipv4 = "private_ipv4";
  public_ipv4 = "public_ipv4";

  if (filename == nullptr)
  {
    helper help;
    help.ec2_inf = this;
    help.items = 3;

    imdsClient.GetUserData([](const Aws::Crt::StringView &resource, int errorCode, void *userData){
      auto h = (helper*)userData;
      std::unique_lock<std::mutex> l(h->mtx);
      if (errorCode == 0) {
        auto ths = h->ec2_inf;
        std::string ud(resource.begin(), resource.size());
        ths->user_data = ud;
      }
      dec(userData);
    }, &help);

    imdsClient.GetInstanceInfo([](const Aws::Crt::Imds::InstanceInfoView &instanceInfo, int errorCode, void *userData){
      auto h = (helper*)userData;
      std::unique_lock<std::mutex> l(h->mtx);
      if (errorCode == 0) {
        auto ths = h->ec2_inf;
        Aws::Crt::Imds::InstanceInfo inf(instanceInfo);
        ths->default_region = inf.region;
        ths->instance_id = inf.instanceId;
        ths->ami_id = inf.imageId;
        ths->private_ipv4 = inf.privateIp;
      }
      dec(userData);
    }, &help);

    imdsClient.GetResource({"/latest/meta-data/public-ipv4"}, [](const Aws::Crt::StringView &resource, int errorCode, void *userData){
      auto h = (helper*)userData;
      std::unique_lock<std::mutex> l(h->mtx);
      if (errorCode == 0) {
        auto ths = h->ec2_inf;
        std::string ip(resource.begin(), resource.size());
        ths->public_ipv4 = ip;
      }
      dec(userData);
    }, &help);

    std::unique_lock<std::mutex> l(help.mtx);
    while (help.items > 0)
      help.cond.wait(l);
  }

  Aws::String region;
  auto rgn = getenv("AWS_DEFAULT_REGION");
  if (rgn)
    default_region = region;

  if (filename != nullptr) {
    json js = json::parse(std::ifstream(filename));
    user_data = js.dump();
  }

  if (user_data.size() != 0)
  {
    try {
      user_data_js = json::parse(user_data);
    }
    catch (...)
    {
      anon_log("user_data does not appear to be json:\n" << user_data);
      user_data.erase();
    }
  }

  auto cwd = getcwd(0, 0);
  root_dir = cwd;
  root_dir += "/resin_root";
  free(cwd);
}

namespace
{
Aws::SDKOptions options;

bool in_ec2(ec2_info &r)
{
  return r.private_ipv4 != "private_ipv4";
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

  // options.loggingOptions.logLevel = Aws::Utils::Logging::LogLevel::Debug;
  // options.loggingOptions.logger_create_fn = []{return std::static_pointer_cast<Aws::Utils::Logging::LogSystemInterface>(std::make_shared<Aws::Utils::Logging::ConsoleLogSystem>(Aws::Utils::Logging::LogLevel::Debug));};

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
