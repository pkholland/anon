/*
 Copyright (c) 2018 Anon authors, see AUTHORS file.
 
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

#ifdef ANON_AWS
#include "aws_client.h"

#include <aws/core/Aws.h>
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/core/auth/AWSCredentialsProviderChain.h>
#include <aws/core/config/AWSProfileConfigLoader.h>
#include <aws/core/utils/logging/LogSystemInterface.h>
#include <aws/core/utils/threading/Executor.h>
#include <aws/core/client/DefaultRetryStrategy.h>
#include <aws/core/client/AWSError.h>
#include <aws/core/auth/STSCredentialsProvider.h>
#include <aws/core/platform/Environment.h>
#include <aws/core/internal/AWSHttpResourceClient.h>
#include <aws/core/utils/logging/LogMacros.h>
#include "aws_http.h"
#include "http_client.h"
#include "dns_lookup.h"

#ifdef ANON_AWS_EC2
#include <aws/ec2/EC2Client.h>
#endif

#ifdef ANON_AWS_DDB
#include <aws/dynamodb/DynamoDBClient.h>
#endif

#ifdef ANON_AWS_DDB_STREAMS
#include <aws/dynamodbstreams/DynamoDBStreamsClient.h>
#endif

#ifdef ANON_AWS_ROUTE53
#include <aws/route53/Route53Client.h>
#endif

#ifdef ANON_AWS_S3
#include <aws/s3/S3Client.h>
#endif

#ifdef ANON_AWS_ACM
#include <aws/acm/ACMClient.h>
#endif

#ifdef ANON_AWS_SQS
#include <aws/sqs/SQSClient.h>
#endif

#ifdef ANON_AWS_ELBV2
#include <aws/elasticloadbalancingv2/ElasticLoadBalancingv2Client.h>
#endif

#ifdef ANON_AWS_ACCEL
#include <aws/globalaccelerator/GlobalAcceleratorClient.h>
#endif

#ifdef ANON_AWS_AUTOSCALING
#include <aws/autoscaling/AutoScalingClient.h>
#endif

#ifdef ANON_AWS_COGNITO
#include <aws/cognito-identity/CognitoIdentityClient.h>
#endif

#ifdef ANON_AWS_SNS
#include <aws/sns/SNSClient.h>
#endif

#ifdef ANON_AWS_SES
#include <aws/email/SESClient.h>
#endif

namespace Aws
{
namespace Client
{
Aws::String ComputeUserAgentString();
}
} // namespace Aws

namespace
{

// task executor that runs tasks in a fiber
class aws_executor : public Aws::Utils::Threading::Executor
{
public:
  static std::shared_ptr<aws_executor> singleton;

protected:
  bool SubmitToThread(std::function<void()> &&f) override
  {
    auto stack_size = 128 * 1024 - 256;
    fiber::run_in_fiber(
        [f] {
          f();
        },
        stack_size, "aws SubmitToThread");
    return true;
  }
};

std::shared_ptr<aws_executor> aws_executor::singleton = std::make_shared<aws_executor>();

// class that logs AWS logging entries to anon's logging format/output
#if EXTENSIVE_AWS_LOGS > 1
class logger : public Aws::Utils::Logging::LogSystemInterface
{
public:
  ~logger() {}
  Aws::Utils::Logging::LogLevel GetLogLevel(void) const override { return Aws::Utils::Logging::LogLevel::Debug; }
  void Log(Aws::Utils::Logging::LogLevel logLevel, const char *tag, const char *formatStr, ...) override
  {
    anon_log("Aws Log");
  }

  void LogStream(Aws::Utils::Logging::LogLevel logLevel, const char *tag, const Aws::OStringStream &messageStream) override
  {
    anon_log(tag << " " << messageStream.rdbuf()->str());
  }

  void Flush() override
  {
  }
};
#endif

// we re-implement the retry class so that the sleep function
// is done with a fiber sleep (AWS's class will use a thread sleep)
class fiberRetryStrategy : public Aws::Client::DefaultRetryStrategy
{
public:
  fiberRetryStrategy(long maxRetries = 10, long scaleFactor = 25)
      : Aws::Client::DefaultRetryStrategy(maxRetries, scaleFactor)
  {
  }

  bool ShouldRetry(const Aws::Client::AWSError<Aws::Client::CoreErrors> &error, long attemptedRetries) const override
  {
    auto ret = Aws::Client::DefaultRetryStrategy::ShouldRetry(error, attemptedRetries);
    if (ret) {
      // general strategy on (ddb) "ProvisionedThroughputExceededException" errors is that the normal
      // backoff-and-retry strategy is insufficient because we want whoever triggered the original
      // operation that caused this call to back _way_ off while we wait for the ddb table's
      // application-autoscaling to detect the condition and scale up our capacity.
      auto& name = error.GetExceptionName();
      if (name == "ProvisionedThroughputExceededException")
        ret = false;
      #if EXTENSIVE_AWS_LOGS > 0
      anon_log("retryStrategy::ShouldRetry(\"" << name << "\"," << attemptedRetries << ") returning " << (ret ? "true" : "false"));
      #endif
    }
    #if EXTENSIVE_AWS_LOGS > 0
    else
      anon_log("retryStrategy::ShouldRetry(\"" << error.GetExceptionName() << "\"," << attemptedRetries << ") returning false, "
                                               << "response code: " << (int)error.GetResponseCode() << ", message: " << error.GetMessage());
    #endif

    return ret;
  }

  long CalculateDelayBeforeNextRetry(const Aws::Client::AWSError<Aws::Client::CoreErrors> &error, long attemptedRetries) const override
  {
    auto ret = Aws::Client::DefaultRetryStrategy::CalculateDelayBeforeNextRetry(error, attemptedRetries);
    auto should_sleep = ShouldRetry(error, attemptedRetries) && ret > 0;
#if EXTENSIVE_AWS_LOGS > 0
    std::ostringstream str;
    str << "retryStrategy::CalculateDelayBeforeNextRetry(" << attemptedRetries << "), error: " << (int)error.GetResponseCode() << ": " << error.GetMessage();
    if (should_sleep)
      str << ", sleeping for " << ret << " milliseconds";
    anon_log(str.str());
#endif
    if (should_sleep)
      fiber::msleep(ret);
    // always tell AWS to not sleep (or, sleep 0 milliseconds if it has to sleep)
    return 0;
  }
};

// AWS's EC2MetadataClient client performs a os mutex lock around it's http call
// to fetch the metadata information from http://169.254.169.254.  That isn't legal
// in anon's fiber-based IO calls.  We reimplement that class so that it does
// a fiber mutex lock instead of an os mutex lock
const char EC2_METADATA_CLIENT_LOG_TAG[] = "EC2MetadataClient";
const char EC2_SECURITY_CREDENTIALS_RESOURCE[] = "/latest/meta-data/iam/security-credentials";
const char EC2_IMDS_TOKEN_RESOURCE[] = "/latest/api/token";
const char EC2_IMDS_TOKEN_TTL_DEFAULT_VALUE[] = "21600";
const char EC2_IMDS_TOKEN_TTL_HEADER[] = "x-aws-ec2-metadata-token-ttl-seconds";
const char EC2_IMDS_TOKEN_HEADER[] = "x-aws-ec2-metadata-token";
const char EC2_REGION_RESOURCE[] = "/latest/meta-data/placement/availability-zone";

class fiberEC2MetadataClient : public Aws::Internal::EC2MetadataClient, public std::enable_shared_from_this<fiberEC2MetadataClient>
{
public:
  fiberEC2MetadataClient(const std::string& imds_token, const char *endpoint = "http://169.254.169.254")
      : Aws::Internal::EC2MetadataClient(*aws_get_client_config(aws_get_default_region()), endpoint),
        m_endpoint(endpoint),
        m_token(imds_token)
  {
    auto dns = dns_lookup::get_addrinfo(endpoint + strlen("http://"), 80);
    if (dns.first != 0 || dns.second.size() == 0)
      anon_throw(std::runtime_error, "unsupported imds endpoint: " << endpoint);
    m_sockaddr = dns.second[0];
    m_sockaddr_len = m_sockaddr.sin6_family == AF_INET6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in);
  }

  virtual ~fiberEC2MetadataClient()
  {
  }

  static std::shared_ptr<http_client_response> get_resource(const std::string& message,
    struct sockaddr* sock_addr, socklen_t sock_addr_len)
  {
    while (true) {
      try {
        auto conn = tcp_client::connect(sock_addr, sock_addr_len);
        if (conn.first == 0) {
          *conn.second << message;
          auto re = std::make_shared<http_client_response>();
          re->parse(*conn.second, true);
          return re;
        } else {
          anon_log("imds connect failed: " << conn.first);
        }
      }
      catch(...) {
        anon_log("imds io failed");
      }
      fiber::msleep(1000);
    }
  }

  std::shared_ptr<http_client_response> get_resource(const std::string& message) const
  {
    return get_resource(message, (struct sockaddr*)&m_sockaddr, m_sockaddr_len);
  }

  static std::string trim(const std::string& str)
  {
    if (str.size() == 0)
      return str;
    auto first = 0;
    while (first < str.size() && std::isspace(str[first]))
      ++first;
    auto last = str.size() - 1;
    while (last >= 0 && std::isspace(str[last]))
      --last;
    return str.substr(first, last + 1 - first);
  }

  std::pair<std::string, int> get_imds_token() const
  {
    std::ostringstream oss;
    oss << "PUT " << EC2_IMDS_TOKEN_RESOURCE << " HTTP/1.1\r\n";
    oss << EC2_IMDS_TOKEN_TTL_HEADER << ": " << EC2_IMDS_TOKEN_TTL_DEFAULT_VALUE << "\r\n";
    oss << "\r\n";
    auto message = oss.str();
    while (true) {
      auto re = get_resource(oss.str());
      if (re->status_code != 200 || !re->headers.contains_header(EC2_IMDS_TOKEN_TTL_HEADER)) {
        anon_log("get token returned status code: " << re->status_code);
      } else {
        std::string body_str;
        for (const auto &data : re->body)
          body_str += std::string(&data[0], data.size());
        return std::make_pair(trim(body_str), std::atoi(re->headers.get_header(EC2_IMDS_TOKEN_TTL_HEADER).str().c_str()));
      }
    }
  }

  void update_imds_token()
  {
    auto tok = get_imds_token();
    if (tok.second > 600) {
      std::weak_ptr<fiberEC2MetadataClient> wp = shared_from_this();
      io_dispatch::schedule_task([wp] {
        fiber::run_in_fiber([wp] {
          auto ths = wp.lock();
          if (ths)
            ths->update_imds_token();
        }, fiber::k_default_stack_size, "update_imds_token");
      }, cur_time() + (tok.second - 600));
    }
    fiber_lock l(m_tokenMutex);
    m_token = tok.first;
  }

  using AWSHttpResourceClient::GetResource;

  virtual Aws::String GetResource(const char *resourcePath) const override
  {
    std::string auth;
    {
      auto ths = const_cast<fiberEC2MetadataClient*>(this);
      fiber_lock l(ths->m_tokenMutex);
      auth = ths->m_token;
    }
    auto num_tries = 0;
    while (++num_tries < 5) {

      std::ostringstream oss;
      oss << "GET " << resourcePath << " HTTP/1.1\r\n";
      oss << EC2_IMDS_TOKEN_HEADER << ": " << auth << "\r\n";
      oss << "\r\n";
      auto message = oss.str();

      auto re = get_resource(message);
      if (re->status_code == 200) {
        std::string body_str;
        for (const auto &data : re->body)
          body_str += std::string(&data[0], data.size());
        return body_str;
      }
      else if (re->status_code == 401) {
        anon_log("got a 401 back when loading resource " << resourcePath);
        auto new_tok = get_imds_token();
        auto ths = const_cast<fiberEC2MetadataClient*>(this);
        fiber_lock l(ths->m_tokenMutex);
        auth = ths->m_token = new_tok.first;
      }
    }
    anon_log("failed to get " << resourcePath);
    return "";
  }

  virtual Aws::String GetDefaultCredentials() const override
  {
    auto reply = trim(GetResource(EC2_SECURITY_CREDENTIALS_RESOURCE));
    if (reply.empty())
      return {};
    Aws::Vector<Aws::String> securityCredentials = Aws::Utils::StringUtils::Split(reply, '\n');
    if (securityCredentials.empty())
      return {};
    std::ostringstream oss;
    oss << EC2_SECURITY_CREDENTIALS_RESOURCE << "/" << securityCredentials[0];
    return GetResource(oss.str().c_str());
  }

  virtual Aws::String GetDefaultCredentialsSecurely() const override
  {
    return GetDefaultCredentials();
  }

  virtual Aws::String GetCurrentRegion() const override
  {
    auto trimmedAZString = trim(GetResource(EC2_REGION_RESOURCE));
    Aws::String region;
    region.reserve(trimmedAZString.length());

    bool digitFound = false;
    for (auto character : trimmedAZString)
    {
      if (digitFound && !isdigit(character))
      {
        break;
      }
      if (isdigit(character))
      {
        digitFound = true;
      }

      region.append(1, character);
    }
    return region;
  }

private:
  Aws::String m_endpoint;
  fiber_mutex m_tokenMutex;
  Aws::String m_token;
  struct sockaddr_in6 m_sockaddr;
  socklen_t m_sockaddr_len;
};

// the implementation of this class in Aws itself (InstanceProfileCredentialsProvider)
// is intended to run when the code finds itself running on an ec2 instance.  But that
// implementation locks a system mutex around http api calls, which we don't permit in
// anon.  So this reimplements that class using fiber mutexes intead.
static const char *INSTANCE_LOG_TAG = "fiberInstanceProfileCredentialsProvider";

class fiberInstanceProfileCredentialsProvider : public Aws::Auth::AWSCredentialsProvider
{
public:
  fiberInstanceProfileCredentialsProvider(const std::shared_ptr<fiberEC2MetadataClient>& mdc, long refreshRateMs = Aws::Auth::REFRESH_THRESHOLD)
      : //m_ec2MetadataConfigLoader(Aws::MakeShared<Aws::Config::EC2InstanceProfileConfigLoader>(std::static_pointer_cast<Aws::Internal::EC2MetadataClient>(std::make_shared<fiberEC2MetadataClient>()))),
        m_loadFrequencyMs(refreshRateMs)
  {
    auto loader = std::make_shared<Aws::Config::EC2InstanceProfileConfigLoader>(mdc);
    m_ec2MetadataConfigLoader = loader;
  }

  fiberInstanceProfileCredentialsProvider(const std::shared_ptr<Aws::Config::EC2InstanceProfileConfigLoader> &loader, long refreshRateMs = Aws::Auth::REFRESH_THRESHOLD)
      : m_ec2MetadataConfigLoader(loader),
        m_loadFrequencyMs(refreshRateMs)
  {
  }

  Aws::Auth::AWSCredentials GetAWSCredentials() override
  {
    fiber_lock l(mutex);
    if (IsTimeToRefresh(m_loadFrequencyMs))
      Reload();

    auto profileIter = m_ec2MetadataConfigLoader->GetProfiles().find(Aws::Config::INSTANCE_PROFILE_KEY);
    if (profileIter != m_ec2MetadataConfigLoader->GetProfiles().end())
      return profileIter->second.GetCredentials();

    return Aws::Auth::AWSCredentials();
  }

protected:
  void Reload() override
  {
    m_ec2MetadataConfigLoader->Load();
    AWSCredentialsProvider::Reload();
  }

private:
  std::shared_ptr<Aws::Config::AWSProfileConfigLoader> m_ec2MetadataConfigLoader;
  long m_loadFrequencyMs;
  fiber_mutex mutex;
};

static const char defaultFiberCredentialsProviderChainTag[] = "defaultFiberAWSCredentialsProviderChain";
static const char AWS_ECS_CONTAINER_CREDENTIALS_RELATIVE_URI[] = "AWS_CONTAINER_CREDENTIALS_RELATIVE_URI";
static const char AWS_ECS_CONTAINER_CREDENTIALS_FULL_URI[] = "AWS_CONTAINER_CREDENTIALS_FULL_URI";
static const char AWS_EC2_METADATA_DISABLED[] = "AWS_EC2_METADATA_DISABLED";
static const char AWS_ECS_CONTAINER_AUTHORIZATION_TOKEN[] = "AWS_CONTAINER_AUTHORIZATION_TOKEN";

class defaultFiberAWSCredentialsProviderChain : public Aws::Auth::AWSCredentialsProviderChain
{
public:
  defaultFiberAWSCredentialsProviderChain(const std::shared_ptr<fiberEC2MetadataClient>& mdc)
  {
    AddProvider(Aws::MakeShared<Aws::Auth::EnvironmentAWSCredentialsProvider>(defaultFiberCredentialsProviderChainTag));
    AddProvider(Aws::MakeShared<Aws::Auth::ProfileConfigFileAWSCredentialsProvider>(defaultFiberCredentialsProviderChainTag));
    AddProvider(Aws::MakeShared<Aws::Auth::STSAssumeRoleWebIdentityCredentialsProvider>(defaultFiberCredentialsProviderChainTag));

    //ECS TaskRole Credentials only available when ENVIRONMENT VARIABLE is set
    const auto relativeUri = Aws::Environment::GetEnv(AWS_ECS_CONTAINER_CREDENTIALS_RELATIVE_URI);

    const auto absoluteUri = Aws::Environment::GetEnv(AWS_ECS_CONTAINER_CREDENTIALS_FULL_URI);

    const auto ec2MetadataDisabled = Aws::Environment::GetEnv(AWS_EC2_METADATA_DISABLED);

    if (!relativeUri.empty())
    {
      AddProvider(Aws::MakeShared<Aws::Auth::TaskRoleCredentialsProvider>(defaultFiberCredentialsProviderChainTag, relativeUri.c_str()));
    }
    else if (!absoluteUri.empty())
    {
      const auto token = Aws::Environment::GetEnv(AWS_ECS_CONTAINER_AUTHORIZATION_TOKEN);
      AddProvider(Aws::MakeShared<Aws::Auth::TaskRoleCredentialsProvider>(defaultFiberCredentialsProviderChainTag,
                                                                          absoluteUri.c_str(), token.c_str()));
    }
    else if (Aws::Utils::StringUtils::ToLower(ec2MetadataDisabled.c_str()) != "true")
    {
      AddProvider(Aws::MakeShared<fiberInstanceProfileCredentialsProvider>(defaultFiberCredentialsProviderChainTag, mdc));
    }
  }
};

Aws::SDKOptions aws_options;
std::shared_ptr<Aws::Auth::AWSCredentialsProvider> aws_cred_prov;
std::string aws_default_region;
std::shared_ptr<fiberEC2MetadataClient> aws_metadata_client;

} // namespace

void aws_client_init()
{
  aws_options.httpOptions.httpClientFactory_create_fn = [] { return std::static_pointer_cast<Aws::Http::HttpClientFactory>(std::make_shared<aws_http_client_factory>()); };
#if EXTENSIVE_AWS_LOGS > 1
  aws_options.loggingOptions.logLevel = Aws::Utils::Logging::LogLevel::Info;
  aws_options.loggingOptions.logger_create_fn = [] { return std::static_pointer_cast<Aws::Utils::Logging::LogSystemInterface>(std::make_shared<logger>()); };
#endif
  Aws::InitAPI(aws_options);

  auto specific_profile = true;
  const char* aws_profile = getenv("AWS_PROFILE");
  if (!aws_profile)
  {
    aws_profile = "default";
    specific_profile = false;
  }

  // basically, if you specified a specific profile to use and we can't find
  // the credentials file, or that file doesn't have the profile you are asking for,
  // we will revert to using the default credential chain mechanism.
  if (specific_profile) {
    auto pfn = Aws::Auth::ProfileConfigFileAWSCredentialsProvider::GetCredentialsProfileFilename();
    Aws::Config::AWSConfigFileProfileConfigLoader loader(pfn);
    if (!loader.Load())
      specific_profile = false;
    else {
      auto profiles = loader.GetProfiles();
      auto profile_it = profiles.find(aws_profile);
      if (profile_it != profiles.end()) {
        aws_default_region = profile_it->second.GetRegion();
      }
      else
        specific_profile = false;
    }
  }
  if (specific_profile) {
    aws_cred_prov = std::make_shared<Aws::Auth::ProfileConfigFileAWSCredentialsProvider>(aws_profile);
  }
  else {

    // sadly, complicated bootstrapping logic...
    // we need to get the region we are running in in order to construct an aws "client configuration",
    // object - which is needed by "credentials provider".  If we don't supply a custom one to
    // the credentials provider ctor, then the credentials provider ctor will construct and use
    // a default one - which doesn't use all of our fiber etc... logic.  But the credentials provider
    // logic is really just the code that reads the metadata from 169.254.169.254.  So here we
    // boostrap that logic

    auto conn = std::make_pair<int, std::unique_ptr<fiber_pipe>>(-1, {});
    auto dns = dns_lookup::get_addrinfo("169.254.169.254", 80);
    if (dns.first != 0 || dns.second.size() == 0)
      anon_throw(std::runtime_error, "can't happen");
    auto sockaddr = dns.second[0];
    auto sockaddr_len = sockaddr.sin6_family == AF_INET6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in);
    if (getenv("ANON_OUTSIDE_EC2") == nullptr)
    {
      conn = tcp_client::connect((struct sockaddr*)&sockaddr, sockaddr_len, false);
    }
    if (conn.first == 0) {

      // first get the token needed to get any metadata
      std::ostringstream oss;
      oss << "PUT " << EC2_IMDS_TOKEN_RESOURCE << " HTTP/1.1\r\n";
      oss << EC2_IMDS_TOKEN_TTL_HEADER << ": " << EC2_IMDS_TOKEN_TTL_DEFAULT_VALUE << "\r\n";
      oss << "\r\n";
      auto message = oss.str();

      std::string imds_tok;
      int imds_valid_seconds = 0;
      while (true) {
        conn = tcp_client::connect((struct sockaddr*)&sockaddr, sockaddr_len);
        if (conn.first == 0) {
          auto re = fiberEC2MetadataClient::get_resource(message, (struct sockaddr*)&sockaddr, sockaddr_len);
          if (re->status_code != 200 || !re->headers.contains_header(EC2_IMDS_TOKEN_TTL_HEADER)) {
            anon_log("get token returned status code: " << re->status_code);
          } else {
            std::string body_str;
            for (const auto &data : re->body)
              body_str += std::string(&data[0], data.size());
            imds_tok = fiberEC2MetadataClient::trim(body_str);
            imds_valid_seconds = std::atoi(re->headers.get_header(EC2_IMDS_TOKEN_TTL_HEADER).str().c_str());
            if (imds_valid_seconds < 1200)
              anon_throw(std::runtime_error, "unusably small imds token expiration time: " << imds_valid_seconds);
            break;
          }
        }
        fiber::msleep(1000);
      }

      auto region_ = getenv("AWS_DEFAULT_REGION");
      if (!region_) {

        // if the region env variable isn't set, read it from metadata
        oss.str("");
        oss.clear();
        oss << "GET " << EC2_REGION_RESOURCE << " HTTP/1.1\r\n";
        oss << EC2_IMDS_TOKEN_HEADER << ": " << imds_tok << "\r\n";
        oss << "\r\n";
        message = oss.str();

        std::string avzone;
        while (true) {
          conn = tcp_client::connect((struct sockaddr*)&sockaddr, sockaddr_len);
          if (conn.first == 0) {
            auto re = fiberEC2MetadataClient::get_resource(message, (struct sockaddr*)&sockaddr, sockaddr_len);
            if (re->status_code == 200) {
              avzone = "";
              for (const auto &data : re->body)
                avzone += std::string(&data[0], data.size());
              avzone = fiberEC2MetadataClient::trim(avzone);
              break;
            }
          }
          fiber::msleep(1000);
        }

        aws_default_region.reserve(avzone.length());
        bool digitFound = false;
        for (auto character : avzone) {
          if (digitFound && !isdigit(character))
            break;
          if (isdigit(character))
            digitFound = true;
          aws_default_region.append(1, character);
        }

        // if this isn't set then the stupid aws std ctor's
        // to to repeat the logic above - only with their own
        // threads and such - before we are able to set the
        // client configuration options that tell it to use our
        // threading
        setenv("AWS_DEFAULT_REGION", aws_default_region.c_str(), 1);
      } else
        aws_default_region = region_;

      aws_metadata_client = std::make_shared<fiberEC2MetadataClient>(imds_tok);

      // update the imds token again 10 min before it is scheduled to expire
      std::weak_ptr<fiberEC2MetadataClient> wp = aws_metadata_client->shared_from_this();
      io_dispatch::schedule_task([wp] {
        fiber::run_in_fiber([wp] {
          auto mc = wp.lock();
          if (mc)
            mc->update_imds_token();
        }, fiber::k_default_stack_size, "aws_client_init");
      }, cur_time() + imds_valid_seconds - 600);

    } else {
      // we don't appear to be in ec2, so hard code a few things (mostly for testing)
      const char* region_ = getenv("AWS_DEFAULT_REGION");
      if (!region_)
        region_ = "us-east-1";
      aws_default_region = region_;
    }

    aws_cred_prov = std::make_shared<defaultFiberAWSCredentialsProviderChain>(aws_metadata_client);
  }

}

std::shared_ptr<Aws::Auth::AWSCredentialsProvider> aws_get_cred_provider()
{
  return aws_cred_prov;
}

const std::string &aws_get_default_region()
{
  return aws_default_region;
}

void aws_init_client_config(Aws::Client::ClientConfiguration &client_cfg, const std::string &region)
{
  client_cfg.region = region.c_str();
  client_cfg.executor = aws_executor::singleton;
  client_cfg.retryStrategy = std::make_shared<fiberRetryStrategy>();
}

std::shared_ptr<Aws::Client::RetryStrategy> aws_fiber_retry_strategy(long maxRetries, long scaleFactor)
{
  return std::make_shared<fiberRetryStrategy>(maxRetries, scaleFactor);
}

bool aws_in_ec2()
{
  return aws_metadata_client.get() != 0;
}

std::string aws_get_metadata(const std::string& path)
{
  if (aws_in_ec2())
    return aws_metadata_client->GetResource(path.c_str());
  return "";
}


namespace {

fiber_mutex config_mtx;
std::map<std::string, std::shared_ptr<Aws::Client::ClientConfiguration>> config_map;

#ifdef ANON_AWS_EC2
std::map<std::string, std::shared_ptr<Aws::EC2::EC2Client>> ec2_map;
#endif

#ifdef ANON_AWS_DDB
std::map<std::string, std::shared_ptr<Aws::DynamoDB::DynamoDBClient>> ddb_map;
#endif

#ifdef ANON_AWS_DDB_STREAMS
std::map<std::string, std::shared_ptr<Aws::DynamoDBStreams::DynamoDBStreamsClient>> ddb_streams_map;
#endif

#ifdef ANON_AWS_ROUTE53
std::map<std::string, std::shared_ptr<Aws::Route53::Route53Client>> r53_map;
#endif

#ifdef ANON_AWS_S3
std::map<std::string, std::shared_ptr<Aws::S3::S3Client>> s3_map;
#endif

#ifdef ANON_AWS_ACM
std::map<std::string, std::shared_ptr<Aws::ACM::ACMClient>> acm_map;
#endif

#ifdef ANON_AWS_SQS
std::map<std::string, std::shared_ptr<Aws::SQS::SQSClient>> sqs_map;
#endif

#ifdef ANON_AWS_ELBV2
std::map<std::string, std::shared_ptr<Aws::ElasticLoadBalancingv2::ElasticLoadBalancingv2Client>> elbv2_map;
#endif

#ifdef ANON_AWS_ACCEL
std::map<std::string, std::shared_ptr<Aws::GlobalAccelerator::GlobalAcceleratorClient>> accel_map;
#endif

#ifdef ANON_AWS_AUTOSCALING
std::map<std::string, std::shared_ptr<Aws::AutoScaling::AutoScalingClient>> auto_map;
#endif

#ifdef ANON_AWS_COGNITO
std::map<std::string, std::shared_ptr<Aws::CognitoIdentity::CognitoIdentityClient>> cognito_map;
#endif

#ifdef ANON_AWS_SNS
std::map<std::string, std::shared_ptr<Aws::SNS::SNSClient>> sns_map;
#endif

#ifdef ANON_AWS_SES
std::map<std::string, std::shared_ptr<Aws::SES::SESClient>> ses_map;
#endif

std::shared_ptr<Aws::Client::ClientConfiguration> aws_get_client_config_nl(const std::string& region)
{
  if (config_map.find(region) == config_map.end()) {
    auto cm = std::make_shared<Aws::Client::ClientConfiguration>();
    aws_init_client_config(*cm, region);
    config_map.insert(std::make_pair(region, std::move(cm)));
  }
  return config_map[region];
}

}

std::shared_ptr<Aws::Client::ClientConfiguration> aws_get_client_config(const std::string& region)
{
  fiber_lock l(config_mtx);
  return aws_get_client_config_nl(region);
}

#ifdef ANON_AWS_EC2
std::shared_ptr<Aws::EC2::EC2Client> aws_get_ec2_client(const std::string& region)
{
  fiber_lock l(config_mtx);
  if (ec2_map.find(region) == ec2_map.end()) {
    Aws::EC2::EC2ClientConfiguration config;
    aws_init_client_config(config, region);
    auto client =  std::make_shared<Aws::EC2::EC2Client>(
      aws_get_cred_provider(),
      std::make_shared<Aws::EC2::EC2EndpointProvider>(),
      config);
    ec2_map.emplace(std::make_pair(region, std::move(client)));
  }
  return ec2_map[region];
}
#endif

#ifdef ANON_AWS_DDB
std::shared_ptr<Aws::DynamoDB::DynamoDBClient> aws_get_ddb_client(const std::string& region)
{
  fiber_lock l(config_mtx);
  if (ddb_map.find(region) == ddb_map.end()) {
    auto client = std::make_shared<Aws::DynamoDB::DynamoDBClient>(
      aws_get_cred_provider(),
      std::make_shared<Aws::DynamoDB::DynamoDBEndpointProvider>(),
      *aws_get_client_config_nl(region));
    ddb_map.emplace(std::make_pair(region, std::move(client)));
  }
  return ddb_map[region];
}
#endif

#ifdef ANON_AWS_DDB_STREAMS
std::shared_ptr<Aws::DynamoDBStreams::DynamoDBStreamsClient> aws_get_ddb_streams_client(const std::string& region)
{
  fiber_lock l(config_mtx);
  if (ddb_streams_map.find(region) == ddb_streams_map.end()) {
    auto client = std::make_shared<Aws::DynamoDBStreams::DynamoDBStreamsClient>(
      aws_get_cred_provider(),
      std::make_shared<Aws::DynamoDBStreams::DynamoDBStreamsEndpointProvider>(),
      *aws_get_client_config_nl(region));
    ddb_streams_map.emplace(std::make_pair(region, std::move(client)));
  }
  return ddb_streams_map[region];
}
#endif

#ifdef ANON_AWS_ROUTE53
std::shared_ptr<Aws::Route53::Route53Client> aws_get_r53_client()
{
  fiber_lock l(config_mtx);
  std::string region = "us-east-1";
  if (r53_map.find(region) == r53_map.end()) {
    auto client = std::make_shared<Aws::Route53::Route53Client>(
      aws_get_cred_provider(),
      std::make_shared<Aws::Route53::Route53EndpointProvider>(),
      *aws_get_client_config_nl(region));
    r53_map.emplace(std::make_pair(region, std::move(client)));
  }
  return r53_map[region];
}
#endif

#ifdef ANON_AWS_S3
std::shared_ptr<Aws::S3::S3Client> aws_get_s3_client(const std::string& region)
{
  fiber_lock l(config_mtx);
  if (s3_map.find(region) == s3_map.end()) {
    auto client = std::make_shared<Aws::S3::S3Client>(
      aws_get_cred_provider(),
      std::make_shared<Aws::S3::S3EndpointProvider>(),
      *aws_get_client_config_nl(region));
    s3_map.emplace(std::make_pair(region,std::move(client)));
  }
  return s3_map[region];
}
#endif

#ifdef ANON_AWS_ACM
std::shared_ptr<Aws::ACM::ACMClient> aws_get_acm_client(const std::string& region)
{
  fiber_lock l(config_mtx);
  if (acm_map.find(region) == acm_map.end()) {
    auto client = std::make_shared<Aws::ACM::ACMClient>(
      aws_get_cred_provider(),
      std::make_shared<Aws::ACM::ACMEndpointProvider>(),
      *aws_get_client_config_nl(region));
    acm_map.emplace(std::make_pair(region, std::move(client)));
  }
  return acm_map[region];
}
#endif

#ifdef ANON_AWS_SQS
std::shared_ptr<Aws::SQS::SQSClient> aws_get_sqs_client(const std::string& region)
{
  fiber_lock l(config_mtx);
  if (sqs_map.find(region) == sqs_map.end()) {
    auto client = std::make_shared<Aws::SQS::SQSClient>(
      aws_get_cred_provider(),
      std::make_shared<Aws::SQS::SQSEndpointProvider>(),
      *aws_get_client_config_nl(region));
    sqs_map.emplace(std::make_pair(region, std::move(client)));
  }
  return sqs_map[region];
}
#endif

#ifdef ANON_AWS_ELBV2
std::shared_ptr<Aws::ElasticLoadBalancingv2::ElasticLoadBalancingv2Client>
aws_get_elbv2_client(const std::string& region)
{
  fiber_lock l(config_mtx);
  if (elbv2_map.find(region) == elbv2_map.end()) {
    auto client = std::make_shared<Aws::ElasticLoadBalancingv2::ElasticLoadBalancingv2Client>(
      aws_get_cred_provider(),
      std::make_shared<Aws::ElasticLoadBalancingv2::ElasticLoadBalancingv2EndpointProvider>(),
      *aws_get_client_config_nl(region));
    elbv2_map.emplace(std::make_pair(region, std::move(client)));
  }
  return elbv2_map[region];
}
#endif

#ifdef ANON_AWS_ACCEL
std::shared_ptr<Aws::GlobalAccelerator::GlobalAcceleratorClient>
aws_get_accel_client()
{
  fiber_lock l(config_mtx);
  std::string region = "us-west-2";
  if (accel_map.find(region) == accel_map.end()) {
    auto client = std::make_shared<Aws::GlobalAccelerator::GlobalAcceleratorClient>(aws_get_cred_provider(), *aws_get_client_config_nl(region));
    accel_map.emplace(std::make_pair(region, std::move(client)));
  }
  return accel_map[region];
}
#endif

#ifdef ANON_AWS_AUTOSCALING
std::shared_ptr<Aws::AutoScaling::AutoScalingClient>
aws_get_autoscaling_client(const std::string& region)
{
  fiber_lock l(config_mtx);
  if (auto_map.find(region) == auto_map.end()) {
    auto client = std::make_shared<Aws::AutoScaling::AutoScalingClient>(aws_get_cred_provider(), *aws_get_client_config_nl(region));
    auto_map.emplace(std::make_pair(region, std::move(client)));
  }
  return auto_map[region];
}
#endif

#ifdef ANON_AWS_COGNITO
std::shared_ptr<Aws::CognitoIdentity::CognitoIdentityClient>
aws_get_cognito_client(const std::string& region)
{
  fiber_lock l(config_mtx);
  if (cognito_map.find(region) == cognito_map.end()) {
    auto client = std::make_shared<Aws::CognitoIdentity::CognitoIdentityClient>(aws_get_cred_provider(), *aws_get_client_config_nl(region));
    cognito_map.emplace(std::make_pair(region, std::move(client)));
  }
  return cognito_map[region];
}
#endif

#ifdef ANON_AWS_SNS
std::shared_ptr<Aws::SNS::SNSClient>
aws_get_sns_client(const std::string& region)
{
  fiber_lock l(config_mtx);
  if (sns_map.find(region) == sns_map.end()) {
    auto client = std::make_shared<Aws::SNS::SNSClient>(aws_get_cred_provider(), std::make_shared<Aws::SNS::SNSEndpointProvider>(), *aws_get_client_config_nl(region));
    sns_map.emplace(std::make_pair(region, std::move(client)));
  }
  return sns_map[region];
}
#endif

#ifdef ANON_AWS_SES
std::shared_ptr<Aws::SES::SESClient>
aws_get_ses_client(const std::string& region)
{
  fiber_lock l(config_mtx);
  if (ses_map.find(region) == ses_map.end()) {
    auto client = std::make_shared<Aws::SES::SESClient>(aws_get_cred_provider(), *aws_get_client_config_nl(region));
    ses_map.emplace(std::make_pair(region, std::move(client)));
  }
  return ses_map[region];
}
#endif

void aws_client_term()
{
  #ifdef ANON_AWS_EC2
  ec2_map.clear();
  #endif

  #ifdef ANON_AWS_DDB
  ddb_map.clear();
  #endif

  #ifdef ANON_AWS_DDB_STREAMS
  ddb_streams_map.clear();
  #endif

  #ifdef ANON_AWS_ROUTE53
  r53_map.clear();
  #endif

  #ifdef ANON_AWS_S3
  s3_map.clear();
  #endif

  #ifdef ANON_AWS_ACM
  acm_map.clear();
  #endif

  #ifdef ANON_AWS_SQS
  sqs_map.clear();
  #endif

  #ifdef ANON_AWS_ELBV2
  elbv2_map.clear();
  #endif

  #ifdef ANON_AWS_ACCEL
  accel_map.clear();
  #endif

  #ifdef ANON_AWS_AUTOSCALING
  auto_map.clear();
  #endif

  #ifdef ANON_AWS_COGNITO
  cognito_map.clear();
  #endif

  #ifdef ANON_AWS_SNS
  sns_map.clear();
  #endif

  #ifdef ANON_AWS_SES
  ses_map.clear();
  #endif

  Aws::ShutdownAPI(aws_options);
}

#endif
