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
  bool ShouldRetry(const Aws::Client::AWSError<Aws::Client::CoreErrors> &error, long attemptedRetries) const
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

  long CalculateDelayBeforeNextRetry(const Aws::Client::AWSError<Aws::Client::CoreErrors> &error, long attemptedRetries) const
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

class fiberEC2MetadataClient : public Aws::Internal::EC2MetadataClient
{
public:
  fiberEC2MetadataClient(const char *endpoint = "http://169.254.169.254")
      : Aws::Internal::EC2MetadataClient(EC2_METADATA_CLIENT_LOG_TAG),
        m_endpoint(endpoint),
        m_tokenRequired(true)
  {
  }

  fiberEC2MetadataClient(const Aws::Client::ClientConfiguration &clientConfiguration, const char *endpoint = "http://169.254.169.254")
      : Aws::Internal::EC2MetadataClient(clientConfiguration, EC2_METADATA_CLIENT_LOG_TAG),
        m_endpoint(endpoint),
        m_tokenRequired(true)
  {
  }

  fiberEC2MetadataClient &operator=(const fiberEC2MetadataClient &rhs) = delete;
  fiberEC2MetadataClient(const fiberEC2MetadataClient &rhs) = delete;
  fiberEC2MetadataClient &operator=(const fiberEC2MetadataClient &&rhs) = delete;
  fiberEC2MetadataClient(const fiberEC2MetadataClient &&rhs) = delete;

  virtual ~fiberEC2MetadataClient()
  {
  }

  using AWSHttpResourceClient::GetResource;

  virtual Aws::String GetResource(const char *resourcePath) const
  {
    return GetResource(m_endpoint.c_str(), resourcePath, nullptr /*authToken*/);
  }

  virtual Aws::String GetDefaultCredentials() const
  {
    auto ths = const_cast<fiberEC2MetadataClient *>(this);
    fiber_lock locker(ths->m_tokenMutex);
    if (m_tokenRequired)
    {
      locker.unlock();
      return GetDefaultCredentialsSecurely();
    }

    AWS_LOGSTREAM_TRACE(m_logtag.c_str(), "Getting default credentials for ec2 instance");
    auto result = GetResourceWithAWSWebServiceResult(m_endpoint.c_str(), EC2_SECURITY_CREDENTIALS_RESOURCE, nullptr);
    Aws::String credentialsString = result.GetPayload();
    auto httpResponseCode = result.GetResponseCode();

    // Note, if service is insane, it might return 404 for our initial secure call,
    // then when we fall back to insecure call, it might return 401 ask for secure call,
    // Then, SDK might get into a recursive loop call situation between secure and insecure call.
    if (httpResponseCode == Aws::Http::HttpResponseCode::UNAUTHORIZED)
    {
      m_tokenRequired = true;
      return {};
    }
    locker.unlock();

    Aws::String trimmedCredentialsString = Aws::Utils::StringUtils::Trim(credentialsString.c_str());
    if (trimmedCredentialsString.empty())
      return {};

    Aws::Vector<Aws::String> securityCredentials = Aws::Utils::StringUtils::Split(trimmedCredentialsString, '\n');

    AWS_LOGSTREAM_DEBUG(m_logtag.c_str(), "Calling EC2MetadataService resource, " << EC2_SECURITY_CREDENTIALS_RESOURCE
                                                                                  << " returned credential string " << trimmedCredentialsString);

    if (securityCredentials.size() == 0)
    {
      AWS_LOGSTREAM_WARN(m_logtag.c_str(), "Initial call to ec2Metadataservice to get credentials failed");
      return {};
    }

    Aws::StringStream ss;
    ss << EC2_SECURITY_CREDENTIALS_RESOURCE << "/" << securityCredentials[0];
    AWS_LOGSTREAM_DEBUG(m_logtag.c_str(), "Calling EC2MetadataService resource " << ss.str());
    return GetResource(ss.str().c_str());
  }

  virtual Aws::String GetDefaultCredentialsSecurely() const
  {
    auto ths = const_cast<fiberEC2MetadataClient *>(this);
    fiber_lock locker(ths->m_tokenMutex);
    if (!m_tokenRequired)
    {
      locker.unlock();
      return GetDefaultCredentials();
    }

    Aws::StringStream ss;
    ss << m_endpoint << EC2_IMDS_TOKEN_RESOURCE;
    std::shared_ptr<Aws::Http::HttpRequest> tokenRequest(Aws::Http::CreateHttpRequest(ss.str(), Aws::Http::HttpMethod::HTTP_PUT,
                                                                                      Aws::Utils::Stream::DefaultResponseStreamFactoryMethod));
    tokenRequest->SetHeaderValue(EC2_IMDS_TOKEN_TTL_HEADER, EC2_IMDS_TOKEN_TTL_DEFAULT_VALUE);
    auto userAgentString = Aws::Client::ComputeUserAgentString();
    tokenRequest->SetUserAgent(userAgentString);
    AWS_LOGSTREAM_TRACE(m_logtag.c_str(), "Calling EC2MetadataService to get token");
    auto result = GetResourceWithAWSWebServiceResult(tokenRequest);
    Aws::String tokenString = result.GetPayload();
    Aws::String trimmedTokenString = Aws::Utils::StringUtils::Trim(tokenString.c_str());

    if (result.GetResponseCode() == Aws::Http::HttpResponseCode::BAD_REQUEST)
    {
      return {};
    }
    else if (result.GetResponseCode() != Aws::Http::HttpResponseCode::OK || trimmedTokenString.empty())
    {
      m_tokenRequired = false;
      AWS_LOGSTREAM_TRACE(m_logtag.c_str(), "Calling EC2MetadataService to get token failed, falling back to less secure way.");
      return GetDefaultCredentials();
    }
    m_token = trimmedTokenString;
    locker.unlock();
    ss.str("");
    ss << m_endpoint << EC2_SECURITY_CREDENTIALS_RESOURCE;
    std::shared_ptr<Aws::Http::HttpRequest> profileRequest(Aws::Http::CreateHttpRequest(ss.str(), Aws::Http::HttpMethod::HTTP_GET,
                                                                                        Aws::Utils::Stream::DefaultResponseStreamFactoryMethod));
    profileRequest->SetHeaderValue(EC2_IMDS_TOKEN_HEADER, trimmedTokenString);
    profileRequest->SetUserAgent(userAgentString);
    Aws::String profileString = GetResourceWithAWSWebServiceResult(profileRequest).GetPayload();

    Aws::String trimmedProfileString = Aws::Utils::StringUtils::Trim(profileString.c_str());
    Aws::Vector<Aws::String> securityCredentials = Aws::Utils::StringUtils::Split(trimmedProfileString, '\n');

    AWS_LOGSTREAM_DEBUG(m_logtag.c_str(), "Calling EC2MetadataService resource, " << EC2_SECURITY_CREDENTIALS_RESOURCE
                                                                                  << " with token returned profile string " << trimmedProfileString);
    if (securityCredentials.size() == 0)
    {
      AWS_LOGSTREAM_WARN(m_logtag.c_str(), "Calling EC2Metadataservice to get profiles failed");
      return {};
    }

    ss.str("");
    ss << m_endpoint << EC2_SECURITY_CREDENTIALS_RESOURCE << "/" << securityCredentials[0];
    std::shared_ptr<Aws::Http::HttpRequest> credentialsRequest(Aws::Http::CreateHttpRequest(ss.str(), Aws::Http::HttpMethod::HTTP_GET,
                                                                                            Aws::Utils::Stream::DefaultResponseStreamFactoryMethod));
    credentialsRequest->SetHeaderValue(EC2_IMDS_TOKEN_HEADER, trimmedTokenString);
    credentialsRequest->SetUserAgent(userAgentString);
    AWS_LOGSTREAM_DEBUG(m_logtag.c_str(), "Calling EC2MetadataService resource " << ss.str() << " with token.");
    return GetResourceWithAWSWebServiceResult(credentialsRequest).GetPayload();
  }

  virtual Aws::String GetCurrentRegion() const
  {
    AWS_LOGSTREAM_TRACE(m_logtag.c_str(), "Getting current region for ec2 instance");

    Aws::StringStream ss;
    ss << m_endpoint << EC2_REGION_RESOURCE;
    std::shared_ptr<Aws::Http::HttpRequest> regionRequest(Aws::Http::CreateHttpRequest(ss.str(), Aws::Http::HttpMethod::HTTP_GET,
                                                                                       Aws::Utils::Stream::DefaultResponseStreamFactoryMethod));
    {
      auto ths = const_cast<fiberEC2MetadataClient *>(this);
      fiber_lock locker(ths->m_tokenMutex);
      if (m_tokenRequired)
      {
        regionRequest->SetHeaderValue(EC2_IMDS_TOKEN_HEADER, m_token);
      }
    }
    regionRequest->SetUserAgent(Aws::Client::ComputeUserAgentString());
    Aws::String azString = GetResourceWithAWSWebServiceResult(regionRequest).GetPayload();

    if (azString.empty())
    {
      AWS_LOGSTREAM_INFO(m_logtag.c_str(),
                         "Unable to pull region from instance metadata service ");
      return {};
    }

    Aws::String trimmedAZString = Aws::Utils::StringUtils::Trim(azString.c_str());
    AWS_LOGSTREAM_DEBUG(m_logtag.c_str(), "Calling EC2MetadataService resource "
                                              << EC2_REGION_RESOURCE << " , returned credential string " << trimmedAZString);

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

    AWS_LOGSTREAM_INFO(m_logtag.c_str(), "Detected current region as " << region);
    return region;
  }

private:
  Aws::String m_endpoint;
  fiber_mutex m_tokenMutex;
  mutable Aws::String m_token;
  mutable bool m_tokenRequired;
};

// the implementation of this class in Aws itself (InstanceProfileCredentialsProvider)
// is intended to run when the code finds itself running on an ec2 instance.  But that
// implementation locks a system mutex around http api calls, which we don't permit in
// anon.  So this reimplements that class using fiber mutexes intead.
static const char *INSTANCE_LOG_TAG = "fiberInstanceProfileCredentialsProvider";

class fiberInstanceProfileCredentialsProvider : public Aws::Auth::AWSCredentialsProvider
{
public:
  fiberInstanceProfileCredentialsProvider(long refreshRateMs = Aws::Auth::REFRESH_THRESHOLD)
      : //m_ec2MetadataConfigLoader(Aws::MakeShared<Aws::Config::EC2InstanceProfileConfigLoader>(std::static_pointer_cast<Aws::Internal::EC2MetadataClient>(std::make_shared<fiberEC2MetadataClient>()))),
        m_loadFrequencyMs(refreshRateMs)
  {
    auto metaClient = std::static_pointer_cast<Aws::Internal::EC2MetadataClient>(std::make_shared<fiberEC2MetadataClient>());
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
  defaultFiberAWSCredentialsProviderChain()
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
      AddProvider(Aws::MakeShared<fiberInstanceProfileCredentialsProvider>(defaultFiberCredentialsProviderChainTag));
    }
  }
};

Aws::SDKOptions aws_options;
std::shared_ptr<Aws::Auth::AWSCredentialsProvider> aws_cred_prov;
const char *aws_profile;
std::string aws_default_region;

} // namespace

void aws_client_init()
{
  aws_options.httpOptions.httpClientFactory_create_fn = [] { return std::static_pointer_cast<Aws::Http::HttpClientFactory>(std::make_shared<aws_http_client_factory>()); };
#if EXTENSIVE_AWS_LOGS > 1
  aws_options.loggingOptions.logLevel = Aws::Utils::Logging::LogLevel::Info;
  aws_options.loggingOptions.logger_create_fn = [] { return std::static_pointer_cast<Aws::Utils::Logging::LogSystemInterface>(std::make_shared<logger>()); };
#endif
  Aws::InitAPI(aws_options);

  bool specific_profile = true;
  aws_profile = getenv("AWS_PROFILE");
  if (!aws_profile)
  {
    aws_profile = "default";
    specific_profile = false;
  }

  // basically, if you specified a specific profile to use and we can't find
  // the credentials file, or that file doesn't have the profile you are asking for,
  // we will revert to using the default credential chain mechanism.
  if (specific_profile)
  {
    auto pfn = Aws::Auth::ProfileConfigFileAWSCredentialsProvider::GetCredentialsProfileFilename();
    Aws::Config::AWSConfigFileProfileConfigLoader loader(pfn);
    if (!loader.Load())
      specific_profile = false;
    else
    {
      auto profiles = loader.GetProfiles();
      if (profiles.find(aws_profile) == profiles.end())
        specific_profile = false;
    }
  }
  if (specific_profile)
    aws_cred_prov = std::make_shared<Aws::Auth::ProfileConfigFileAWSCredentialsProvider>(aws_profile);
  else
    aws_cred_prov = std::make_shared<defaultFiberAWSCredentialsProviderChain>();

  const char *region_ = getenv("AWS_DEFAULT_REGION");
  if (region_)
    aws_default_region = region_;
  else
  {
    auto pfn = Aws::Auth::ProfileConfigFileAWSCredentialsProvider::GetCredentialsProfileFilename();
    Aws::Config::AWSConfigFileProfileConfigLoader loader(pfn);
    if (loader.Load())
    {
      auto profiles = loader.GetProfiles();
      auto prof = profiles.find(aws_profile);
      if (prof != profiles.end())
        aws_default_region = prof->second.GetRegion().c_str();
    }

    if (aws_default_region.size() == 0)
    {
      bool success = false;
      std::string reply;
      try
      {
        auto epc = endpoint_cluster::create("169.254.169.254", 80);
        auto path = "/latest/meta-data/placement/availability-zone";
        std::ostringstream str;
        str << "GET " << path << " HTTP/1.1\r\n\r\n";
        auto message = str.str();
        epc->with_connected_pipe([&reply, &message, &success](const pipe_t *pipe) -> bool {
          pipe->write(message.c_str(), message.size());
          http_client_response re;
          re.parse(*pipe, true);
          if (re.status_code >= 200 && re.status_code < 300)
          {
            success = true;
            std::ostringstream ret;
            for (const auto &data : re.body)
              ret << std::string(&data[0], data.size());
            reply = ret.str();
          }
          return false;
        });
      }
      catch (...)
      {
      }
      if (success && reply.size() > 1)
        aws_default_region = reply.substr(0, reply.size() - 1).c_str();
      else
        aws_default_region = "us-east-1";
    }
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

namespace {

fiber_mutex config_mtx;
std::map<std::string, Aws::Client::ClientConfiguration> config_map;

//#ifdef ANON_AWS_EC2
std::map<std::string, Aws::EC2::EC2Client> ec2_map;
//#endif

//#ifdef ANON_AWS_DDB
std::map<std::string, std::unique_ptr<Aws::DynamoDB::DynamoDBClient>> ddb_map;
//#endif

//#ifdef ANON_AWS_ROUTE53
std::map<std::string, Aws::Route53::Route53Client> r53_map;
//#endif

//#ifdef ANON_AWS_S3
std::map<std::string, Aws::S3::S3Client> s3_map;
//#endif

//#ifdef ANON_AWS_ACM
std::map<std::string, Aws::ACM::ACMClient> acm_map;
//#endif

//#ifdef ANON_AWS_SQS
std::map<std::string, Aws::SQS::SQSClient> sqs_map;
//#endif

const Aws::Client::ClientConfiguration& aws_get_client_config_nl(const std::string& region)
{
  if (config_map.find(region) == config_map.end())
    aws_init_client_config(config_map[region], region);
  return config_map[region];
}

}

const Aws::Client::ClientConfiguration& aws_get_client_config(const std::string& region)
{
  fiber_lock l(config_mtx);
  return aws_get_client_config_nl(region);
}

//#ifdef ANON_AWS_EC2
const Aws::EC2::EC2Client& aws_get_ec2_client(const std::string& region)
{
  fiber_lock l(config_mtx);
  if (ec2_map.find(region) == ec2_map.end())
    ec2_map.emplace(std::make_pair(region,
      Aws::EC2::EC2Client(aws_get_cred_provider(), aws_get_client_config_nl(region))));
  return ec2_map[region];
}
//#endif

//#ifdef ANON_AWS_DDB
const Aws::DynamoDB::DynamoDBClient& aws_get_ddb_client(const std::string& region)
{
  fiber_lock l(config_mtx);
  if (ddb_map.find(region) == ddb_map.end())
  {
    std::unique_ptr<Aws::DynamoDB::DynamoDBClient>
    c(new Aws::DynamoDB::DynamoDBClient(aws_get_cred_provider(), aws_get_client_config_nl(region)));
    ddb_map[region] = std::move(c);
  }
  return *ddb_map[region];
}
//#endif

//#ifdef ANON_AWS_ROUTE53
const Aws::Route53::Route53Client& aws_get_r53_client()
{
  fiber_lock l(config_mtx);
  std::string region = "us-east-1";
  if (r53_map.find(region) == r53_map.end())
    r53_map.emplace(std::make_pair(region,
      Aws::Route53::Route53Client(aws_get_cred_provider(), aws_get_client_config_nl(region))));
  return r53_map[region];
}
//#endif

//#ifdef ANON_AWS_S3
const Aws::S3::S3Client& aws_get_s3_client(const std::string& region)
{
  fiber_lock l(config_mtx);
  if (s3_map.find(region) == s3_map.end())
    s3_map.emplace(std::make_pair(region,
      Aws::S3::S3Client(aws_get_cred_provider(), aws_get_client_config_nl(region))));
  return s3_map[region];
}
//#endif

//#ifdef ANON_AWS_ACM
const Aws::ACM::ACMClient& aws_get_acm_client(const std::string& region)
{
  fiber_lock l(config_mtx);
  if (acm_map.find(region) == acm_map.end())
    acm_map.emplace(std::make_pair(region,
      Aws::ACM::ACMClient(aws_get_cred_provider(), aws_get_client_config_nl(region))));
  return acm_map[region];
}
//#endif

//#ifdef ANON_AWS_SQS
const Aws::SQS::SQSClient& aws_get_sqs_client(const std::string& region)
{
  fiber_lock l(config_mtx);
  if (sqs_map.find(region) == sqs_map.end())
    sqs_map.emplace(std::make_pair(region,
      Aws::SQS::SQSClient(aws_get_cred_provider(), aws_get_client_config_nl(region))));
  return sqs_map[region];
}
//#endif

#endif