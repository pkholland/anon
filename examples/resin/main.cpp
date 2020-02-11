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
#include "log.h"
#include "resin.h"

using namespace nlohmann;

namespace
{
Aws::SDKOptions options;
std::shared_ptr<Aws::Utils::Threading::Executor> executor;

void init_ec2(ec2_info &r)
{
  Aws::Client::ClientConfiguration config;
  config.executor = executor;
  config.connectTimeoutMs = 100;
  config.httpRequestTimeoutMs = 100;
  config.retryStrategy = std::make_shared<Aws::Client::DefaultRetryStrategy>(2, 10);
  Aws::Internal::EC2MetadataClient client(config);

  auto dfr = getenv("AWS_DEFAULT_REGION");
  if (dfr)
    r.default_region = dfr;
  else
    r.default_region = client.GetCurrentRegion();
  if (r.default_region.size() == 0)
    r.default_region = "us-east-1";

  r.ami_id = client.GetResource("/latest/meta-data/ami-id");
  if (r.ami_id.size() != 0)
  {
    r.instance_id = client.GetResource("/latest/meta-data/instance-id");
    r.host_name = client.GetResource("/latest/meta-data/local-hostname");
    r.private_ipv4 = client.GetResource("/latest/meta-data/local-ipv4");
    r.user_data = client.GetResource("/latest/user-data/");
    if (r.user_data.size() != 0)
      r.user_data_js = json::parse(r.user_data);
  }
}

bool in_ec2(ec2_info &r)
{
  return r.ami_id.size() != 0 && r.instance_id.size() != 0 && r.host_name.size() != 0 && r.private_ipv4.size() != 0;
}

bool has_user_data(ec2_info &r)
{
  return r.user_data.size() != 0;
}

ec2_info ec2i;

} // namespace

extern "C" int main(int argc, char **argv)
{
  anon_log("resin starting");
  int ret = 0;

  Aws::InitAPI(options);
  try
  {
    executor = std::make_shared<Aws::Utils::Threading::PooledThreadExecutor>(4 /*threads in pool*/);
    init_ec2(ec2i);
    if (!in_ec2(ec2i))
      anon_log("resin run outside of ec2, stopping now");
    else if (!has_user_data(ec2i))
      anon_log("resin run without supplying user data, stopping now");
    else
    {
      std::string server_type = ec2i.user_data_js["server_type"];
      if (server_type == "worker")
        run_worker(ec2i);
      else {
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
  executor.reset();
  Aws::ShutdownAPI(options);
  return ret;
}
