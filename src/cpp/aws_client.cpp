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
#include <aws/crt/ImdsClient.h>
#include <aws/core/Globals.h>
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
    auto stack_size = 64 * 1024 - 256;
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

#if EXTENSIVE_AWS_LOGS > 0
  bool ShouldRetry(const Aws::Client::AWSError<Aws::Client::CoreErrors> &error, long attemptedRetries) const override
  {
    auto ret = Aws::Client::DefaultRetryStrategy::ShouldRetry(error, attemptedRetries);
    if (ret)
      anon_log("retryStrategy::ShouldRetry(\"" << error.GetExceptionName() << "\"," << attemptedRetries << ") returning true");
    else
      anon_log("retryStrategy::ShouldRetry(\"" << error.GetExceptionName() << "\"," << attemptedRetries << ") returning false, "
                                               << "response code: " << (int)error.GetResponseCode() << ", message: " << error.GetMessage());
    return ret;
  }
#endif

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
  fiberEC2MetadataClient(const char *endpoint = "http://169.254.169.254")
      : Aws::Internal::EC2MetadataClient(aws_get_client_config(aws_get_default_region()), endpoint),
        m_endpoint(endpoint)
  {
    auto dns = dns_lookup::get_addrinfo(endpoint + strlen("http://"), 80);
    if (dns.first != 0 || dns.second.size() == 0)
      anon_throw(std::runtime_error, "unsupported imds endpoint: " << endpoint);
    m_sockaddr = dns.second[0];
    m_sockaddr_len = m_sockaddr.sin6_family == AF_INET6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in);
    update_imds_token();
  }

  fiberEC2MetadataClient(const Aws::Client::ClientConfiguration &clientConfiguration, const char *endpoint = "http://169.254.169.254")
      : Aws::Internal::EC2MetadataClient(clientConfiguration, endpoint),
        m_endpoint(endpoint)
  {
    auto dns = dns_lookup::get_addrinfo(endpoint + strlen("http://"), 80);
    if (dns.first != 0 || dns.second.size() == 0)
      anon_throw(std::runtime_error, "unsupported imds endpoint: " << endpoint);
    m_sockaddr = dns.second[0];
    m_sockaddr_len = m_sockaddr.sin6_family == AF_INET6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in);
    update_imds_token();
  }

  fiberEC2MetadataClient &operator=(const fiberEC2MetadataClient &rhs) = delete;
  fiberEC2MetadataClient(const fiberEC2MetadataClient &rhs) = delete;
  fiberEC2MetadataClient &operator=(const fiberEC2MetadataClient &&rhs) = delete;
  fiberEC2MetadataClient(const fiberEC2MetadataClient &&rhs) = delete;

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

  std::pair<std::string, int> get_imds_token()
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
        });

        auto ths = wp.lock();
        if (ths) {
          ths->update_imds_token();
        }
      }, cur_time() - (tok.second - 600));
    }
    fiber_lock l(m_tokenMutex);
    m_token = tok.first;
    m_tokCond.notify_all();
  }

  using AWSHttpResourceClient::GetResource;

  virtual Aws::String GetResource(const char *resourcePath) const override
  {
    std::string auth;
    {
      auto ths = const_cast<fiberEC2MetadataClient*>(this);
      fiber_lock l(ths->m_tokenMutex);
      while (ths->m_token.size() == 0)
        ths->m_tokCond.wait(l);
      auth = ths->m_token;
    }
    std::ostringstream oss;
    oss << "GET " << resourcePath << " HTTP/1.1\r\n";
    oss << EC2_IMDS_TOKEN_HEADER << ": " << auth << "\r\n";
    oss << "\r\n";
    auto message = oss.str();
    auto num_tries = 0;
    while (++num_tries < 5) {
      auto re = get_resource(message);
      if (re->status_code == 200) {
        std::string body_str;
        for (const auto &data : re->body)
          body_str += std::string(&data[0], data.size());
        return body_str;
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
  fiber_cond m_tokCond;
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
    auto metaClient = mdc;
    if (!metaClient) {
      Aws::Client::ClientConfiguration cfg;
      aws_init_client_config(cfg, aws_get_default_region());
      metaClient = std::make_shared<fiberEC2MetadataClient>(cfg);
      metaClient->GetDefaultCredentialsSecurely();
    }
    auto loader = std::make_shared<Aws::Config::EC2InstanceProfileConfigLoader>(metaClient);
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
  defaultFiberAWSCredentialsProviderChain(const std::shared_ptr<fiberEC2MetadataClient>& mdc = std::shared_ptr<fiberEC2MetadataClient>())
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
        anon_log("setting provider from profile: " << aws_profile << ", region: " << aws_default_region);
      }
      else
        specific_profile = false;
    }
  }
  if (specific_profile) {
    aws_cred_prov = std::make_shared<Aws::Auth::ProfileConfigFileAWSCredentialsProvider>(aws_profile);
    anon_log("loaded provider from " << aws_profile);
  }
  else {
    const char* region_ = getenv("AWS_DEFAULT_REGION");
    if (!region_) {
      anon_log("no AWS_DEFAULT_REGION environment variable, trying ec2 metadata");

      // sadly, complicated bootstrapping logic...
      // we need to get the region we are running in in order to construct an aws "client configuration",
      // object - which is needed by "credentials provider".  If we don't supply a custom one to
      // the credentials provider ctor, then the credentials provider ctor will construct and use
      // a default one - which doesn't use all of our fiber etc... logic.  But the credentials provider
      // logic is really just the code that reads the metadata from 169.254.169.254.  So here we
      // boostrap that logic

      auto dns = dns_lookup::get_addrinfo("169.254.169.254", 80);
      if (dns.first != 0 || dns.second.size() == 0)
        anon_throw(std::runtime_error, "can't happen");
      auto sockaddr = dns.second[0];
      auto sockaddr_len = sockaddr.sin6_family == AF_INET6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in);
      auto conn = tcp_client::connect((struct sockaddr*)&sockaddr, sockaddr_len, false);
      if (conn.first == 0) {

        // first get the token needed to get any metadata
        std::ostringstream oss;
        oss << "PUT " << EC2_IMDS_TOKEN_RESOURCE << " HTTP/1.1\r\n";
        oss << EC2_IMDS_TOKEN_TTL_HEADER << ": " << EC2_IMDS_TOKEN_TTL_DEFAULT_VALUE << "\r\n";
        oss << "\r\n";
        auto message = oss.str();

        std::string imds_tok;
        while (true) {
          conn = tcp_client::connect((struct sockaddr*)&sockaddr, sockaddr_len);
          if (conn.first == 0) {
            auto re = fiberEC2MetadataClient::get_resource(message, (struct sockaddr*)&sockaddr, sockaddr_len);
            if (re->status_code == 200) {
              imds_tok = "";
              for (const auto &data : re->body)
                imds_tok += std::string(&data[0], data.size());
              imds_tok = fiberEC2MetadataClient::trim(imds_tok);
              break;
            }
          }
          fiber::msleep(1000);
        }

        // now get the current region
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

        aws_metadata_client = std::make_shared<fiberEC2MetadataClient>();
      } else
        aws_default_region = "us-east-1";
    }
    aws_cred_prov = std::make_shared<defaultFiberAWSCredentialsProviderChain>(aws_metadata_client);
  }

}

void aws_client_term()
{
  Aws::ShutdownAPI(aws_options);
}

const std::shared_ptr<Aws::Auth::AWSCredentialsProvider> &aws_get_cred_provider()
{
  return aws_cred_prov;
}

const std::string &aws_get_default_region()
{
  return aws_default_region;
}

std::string aws_get_region_display_name(const std::string& region)
{
  if (region == "us-east-1")
    return "N. Virginia";
  if (region == "us-east-2")
    return "Ohio";
  if (region == "us-west-1")
    return "N. California";
  if (region == "us-west-2")
    return "Oregon";
  if (region == "af-south-1")
    return "Cape Town";
  if (region == "ap-east-1")
    return "Hong Kong";
  if (region == "ap-south-1")
    return "Mumbai";
  if (region == "ap-northeast-1")
    return "Tokyo";
  if (region == "ap-northeast-2")
    return "Seoul";
  if (region == "ap-northeast-3")
    return "Osaka";
  if (region == "ap-southeast-1")
    return "Singapore";
  if (region == "ap-southeast-2")
    return "Sydney";
  if (region == "ca-central-1")
    return "Canada Central";
  if (region == "cn-north-1")
    return "Begijing";
  if (region == "cn-northwest-1")
    return "Ningxia";
  if (region == "eu-central-1")
    return "Frankfurt";
  if (region == "eu-west-1")
    return "Ireland";
  if (region == "eu-west-2")
    return "London";
  if (region == "eu-west-3")
    return "Paris";
  if (region == "eu-south-1")
    return "Milan";
  if (region == "eu-north-1")
    return "Stockholm";
  if (region == "me-south-1")
    return "Bahrain";
  if (region == "sa-east-1")
    return "Sao Paulo";
  return region;
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
std::map<std::string, Aws::Client::ClientConfiguration> config_map;

#ifdef ANON_AWS_EC2
std::map<std::string, Aws::EC2::EC2Client> ec2_map;
#endif

#ifdef ANON_AWS_DDB
std::map<std::string, std::unique_ptr<Aws::DynamoDB::DynamoDBClient>> ddb_map;
#endif

#ifdef ANON_AWS_DDB_STREAMS
std::map<std::string, std::unique_ptr<Aws::DynamoDBStreams::DynamoDBStreamsClient>> ddb_streams_map;
#endif

#ifdef ANON_AWS_ROUTE53
std::map<std::string, Aws::Route53::Route53Client> r53_map;
#endif

#ifdef ANON_AWS_S3
std::map<std::string, Aws::S3::S3Client> s3_map;
#endif

#ifdef ANON_AWS_ACM
std::map<std::string, Aws::ACM::ACMClient> acm_map;
#endif

#ifdef ANON_AWS_SQS
std::map<std::string, Aws::SQS::SQSClient> sqs_map;
#endif

#ifdef ANON_AWS_ELBV2
std::map<std::string, Aws::ElasticLoadBalancingv2::ElasticLoadBalancingv2Client> elbv2_map;
#endif

#ifdef ANON_AWS_ACCEL
std::map<std::string, Aws::GlobalAccelerator::GlobalAcceleratorClient> accel_map;
#endif

#ifdef ANON_AWS_AUTOSCALING
std::map<std::string, Aws::AutoScaling::AutoScalingClient> auto_map;
#endif

#ifdef ANON_AWS_COGNITO
std::map<std::string, Aws::CognitoIdentity::CognitoIdentityClient> cognito_map;
#endif

#ifdef ANON_AWS_SNS
std::map<std::string, Aws::SNS::SNSClient> sns_map;
#endif

#ifdef ANON_AWS_SES
std::map<std::string, Aws::SES::SESClient> ses_map;
#endif

const Aws::Client::ClientConfiguration& aws_get_client_config_nl(const std::string& region)
{
  if (config_map.find(region) == config_map.end()) {
    aws_init_client_config(config_map[region], region);
  }
  return config_map[region];
}

}

const Aws::Client::ClientConfiguration& aws_get_client_config(const std::string& region)
{
  fiber_lock l(config_mtx);
  return aws_get_client_config_nl(region);
}

#ifdef ANON_AWS_EC2
const Aws::EC2::EC2Client& aws_get_ec2_client(const std::string& region)
{
  fiber_lock l(config_mtx);
  if (ec2_map.find(region) == ec2_map.end()) {
    l.unlock();
    auto client =  Aws::EC2::EC2Client(aws_get_cred_provider(), aws_get_client_config_nl(region));
    l.lock();
    if (ec2_map.find(region) == ec2_map.end())
      ec2_map.emplace(std::make_pair(region, std::move(client)));
  }
  return ec2_map[region];
}
#endif

#ifdef ANON_AWS_DDB
const Aws::DynamoDB::DynamoDBClient& aws_get_ddb_client(const std::string& region)
{
  fiber_lock l(config_mtx);
  if (ddb_map.find(region) == ddb_map.end()) {
    l.unlock();
    std::unique_ptr<Aws::DynamoDB::DynamoDBClient>
    c(new Aws::DynamoDB::DynamoDBClient(aws_get_cred_provider(), aws_get_client_config_nl(region)));
    l.lock();
    if (ddb_map.find(region) == ddb_map.end())
      ddb_map[region] = std::move(c);
  }
  return *ddb_map[region];
}
#endif

#ifdef ANON_AWS_DDB_STREAMS
const Aws::DynamoDBStreams::DynamoDBStreamsClient& aws_get_ddb_streams_client(const std::string& region)
{
  fiber_lock l(config_mtx);
  if (ddb_streams_map.find(region) == ddb_streams_map.end()) {
    l.unlock();
    std::unique_ptr<Aws::DynamoDBStreams::DynamoDBStreamsClient>
    c(new Aws::DynamoDBStreams::DynamoDBStreamsClient(aws_get_cred_provider(), aws_get_client_config_nl(region)));
    l.lock();
    if (ddb_streams_map.find(region) == ddb_streams_map.end())
      ddb_streams_map[region] = std::move(c);
  }
  return *ddb_streams_map[region];
}
#endif

#ifdef ANON_AWS_ROUTE53
const Aws::Route53::Route53Client& aws_get_r53_client()
{
  fiber_lock l(config_mtx);
  std::string region = "us-east-1";
  if (r53_map.find(region) == r53_map.end()) {
    l.unlock();
    auto client = Aws::Route53::Route53Client(aws_get_cred_provider(), aws_get_client_config_nl(region));
    l.lock();
    if (r53_map.find(region) == r53_map.end())
      r53_map.emplace(std::make_pair(region, std::move(client)));
  }
  return r53_map[region];
}
#endif

#ifdef ANON_AWS_S3
const Aws::S3::S3Client& aws_get_s3_client(const std::string& region)
{
  fiber_lock l(config_mtx);
  if (s3_map.find(region) == s3_map.end()) {
    l.unlock();
    auto client = Aws::S3::S3Client(aws_get_cred_provider(), aws_get_client_config_nl(region));
    l.lock();
    if (s3_map.find(region) == s3_map.end())
      s3_map.emplace(std::make_pair(region,std::move(client)));
  }
  return s3_map[region];
}
#endif

#ifdef ANON_AWS_ACM
const Aws::ACM::ACMClient& aws_get_acm_client(const std::string& region)
{
  fiber_lock l(config_mtx);
  if (acm_map.find(region) == acm_map.end()) {
    l.unlock();
    auto client = Aws::ACM::ACMClient(aws_get_cred_provider(), aws_get_client_config_nl(region));
    l.lock();
    if (acm_map.find(region) == acm_map.end())
      acm_map.emplace(std::make_pair(region, std::move(client)));
  }
  return acm_map[region];
}
#endif

#ifdef ANON_AWS_SQS
const Aws::SQS::SQSClient& aws_get_sqs_client(const std::string& region)
{
  fiber_lock l(config_mtx);
  if (sqs_map.find(region) == sqs_map.end()) {
    l.unlock();
    auto client = Aws::SQS::SQSClient(aws_get_cred_provider(), aws_get_client_config_nl(region));
    l.lock();
    if (sqs_map.find(region) == sqs_map.end())
      sqs_map.emplace(std::make_pair(region, std::move(client)));
  }
  return sqs_map[region];
}
#endif

#ifdef ANON_AWS_ELBV2
const Aws::ElasticLoadBalancingv2::ElasticLoadBalancingv2Client&
aws_get_elbv2_client(const std::string& region)
{
  fiber_lock l(config_mtx);
  if (elbv2_map.find(region) == elbv2_map.end()) {
    l.unlock();
    auto client = Aws::ElasticLoadBalancingv2::ElasticLoadBalancingv2Client(aws_get_cred_provider(), aws_get_client_config_nl(region));
    l.lock();
    if (elbv2_map.find(region) == elbv2_map.end())
      elbv2_map.emplace(std::make_pair(region, std::move(client)));
  }
  return elbv2_map[region];
}
#endif

#ifdef ANON_AWS_ACCEL
const Aws::GlobalAccelerator::GlobalAcceleratorClient&
aws_get_accel_client()
{
  fiber_lock l(config_mtx);
  std::string region = "us-west-2";
  if (accel_map.find(region) == accel_map.end()) {
    l.unlock();
    auto client = Aws::GlobalAccelerator::GlobalAcceleratorClient(aws_get_cred_provider(), aws_get_client_config_nl(region));
    l.lock();
    if (accel_map.find(region) == accel_map.end())
      accel_map.emplace(std::make_pair(region, std::move(client)));
  }
  return accel_map[region];
}
#endif

#ifdef ANON_AWS_AUTOSCALING
const Aws::AutoScaling::AutoScalingClient&
aws_get_autoscaling_client(const std::string& region)
{
  fiber_lock l(config_mtx);
  if (auto_map.find(region) == auto_map.end())
    auto_map.emplace(std::make_pair(region,
      Aws::AutoScaling::AutoScalingClient(aws_get_cred_provider(), aws_get_client_config_nl(region))));
  return auto_map[region];
}
#endif

#ifdef ANON_AWS_COGNITO
const Aws::CognitoIdentity::CognitoIdentityClient&
aws_get_cognito_client(const std::string& region)
{
  fiber_lock l(config_mtx);
  if (cognito_map.find(region) == cognito_map.end())
    cognito_map.emplace(std::make_pair(region,
      Aws::CognitoIdentity::CognitoIdentityClient(aws_get_cred_provider(), aws_get_client_config_nl(region))));
  return cognito_map[region];
}
#endif

#ifdef ANON_AWS_SNS
const Aws::SNS::SNSClient&
aws_get_sns_client(const std::string& region)
{
  fiber_lock l(config_mtx);
  if (sns_map.find(region) == sns_map.end())
    sns_map.emplace(std::make_pair(region,
      Aws::SNS::SNSClient(aws_get_cred_provider(), aws_get_client_config_nl(region))));
  return sns_map[region];
}
#endif

#ifdef ANON_AWS_SES
const Aws::SES::SESClient&
aws_get_ses_client(const std::string& region)
{
  fiber_lock l(config_mtx);
  if (ses_map.find(region) == ses_map.end())
    ses_map.emplace(std::make_pair(region,
      Aws::SES::SESClient(aws_get_cred_provider(), aws_get_client_config_nl(region))));
  return ses_map[region];
}
#endif



#endif
