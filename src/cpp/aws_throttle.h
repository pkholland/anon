/*
 Copyright (c) 2020 Anon authors, see AUTHORS file.
 
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

#pragma once

#ifdef ANON_AWS

#include "log.h"
#include <functional>
#include <exception>
#include <aws/core/http/HttpResponse.h>
#ifdef ANON_AWS_EC2
#include <aws/ec2/EC2Errors.h>
#endif
#ifdef ANON_AWS_DDB
#include <aws/dynamodb/DynamoDBErrors.h>
#endif
#ifdef ANON_AWS_ROUTE53
#include <aws/route53/Route53Errors.h>
#endif
#ifdef ANON_AWS_ACM
#include <aws/acm/ACMErrors.h>
#endif
#ifdef ANON_AWS_S3
#include <aws/s3/S3Errors.h>
#endif
#ifdef ANON_AWS_ELBV2
#include <aws/elasticloadbalancingv2/ElasticLoadBalancingv2Errors.h>
#endif
#ifdef ANON_AWS_SNS
#include <aws/sns/SNSErrors.h>
#endif

class aws_throttle_error : public std::runtime_error
{
public:
  aws_throttle_error(const std::string &msg)
      : std::runtime_error(msg)
  {
  }
};

template<typename E>
bool generic_retry(E e)
{
  auto ce = (Aws::Client::CoreErrors)e;
  return
       ce == Aws::Client::CoreErrors::INTERNAL_FAILURE
    || ce == Aws::Client::CoreErrors::THROTTLING
    || ce == Aws::Client::CoreErrors::REQUEST_EXPIRED
    || ce == Aws::Client::CoreErrors::SERVICE_UNAVAILABLE
    || ce == Aws::Client::CoreErrors::RESOURCE_NOT_FOUND
    || ce == Aws::Client::CoreErrors::REQUEST_TIME_TOO_SKEWED
    || ce == Aws::Client::CoreErrors::REQUEST_TIMEOUT
    || ce == Aws::Client::CoreErrors::SLOW_DOWN;
}

template<typename E>
bool generic_retry_2(const E& e)
{
  return false;
}

#ifdef ANON_AWS_EC2
inline bool generic_retry(Aws::EC2::EC2Errors e)
{
  return generic_retry((Aws::Client::CoreErrors)e)
    || e == Aws::EC2::EC2Errors::INVALID_DEVICE__IN_USE
    || e == Aws::EC2::EC2Errors::INVALID_GROUP__IN_USE
    || e == Aws::EC2::EC2Errors::INVALID_I_P_ADDRESS__IN_USE
    || e == Aws::EC2::EC2Errors::INVALID_PLACEMENT_GROUP__IN_USE
    || e == Aws::EC2::EC2Errors::INVALID_SNAPSHOT__IN_USE
    || e == Aws::EC2::EC2Errors::VOLUME_IN_USE;
}

inline bool generic_retry_2(const Aws::EC2::EC2Error& e) {
  return e.GetExceptionName() == "RequestLimitExceeded";
}
#endif

#ifdef ANON_AWS_ELBV2
inline bool generic_retry(Aws::ElasticLoadBalancingv2::ElasticLoadBalancingv2Errors e)
{
  return generic_retry((Aws::Client::CoreErrors)e)
    || e == Aws::ElasticLoadBalancingv2::ElasticLoadBalancingv2Errors::RESOURCE_IN_USE;
}
#endif

#ifdef ANON_AWS_SNS
inline bool generic_retry(Aws::SNS::SNSErrors e)
{
  return generic_retry((Aws::Client::CoreErrors)e)
    || e == Aws::SNS::SNSErrors::K_M_S_THROTTLING;
}
#endif

#ifdef ANON_AWS_DDB
inline bool generic_retry(Aws::DynamoDB::DynamoDBErrors e)
{
  return generic_retry((Aws::Client::CoreErrors)e)
    || e == Aws::DynamoDB::DynamoDBErrors::PROVISIONED_THROUGHPUT_EXCEEDED;
}
#endif

/*
  the "ignorable duplicate" problem...

  Any network system that has a built-in retry on failure comes with
  the cost that it causes cases where A asks B to do something, B does it
  but then there is a failure in B reporting back to A that it has been done.
  So, when we ask AWS to create something like a Route in a RouteTable it
  will occasionally hit the condition where AWS creates that route, but then
  something goes wrong in reporting back to us that it has been created.
  This is indistinguishable to us from the case where AWS fails to recieve
  the message and so does not create the Route.  We always retry these
  cases where there has been some network problem during the request.
  That will occasionally cause duplicate resources to be requested.  In some
  classes of AWS resources (Route's in a RouteTable are an example), the
  request to create the duplicate causes AWS to return an error - as
  opposed to creating a second resource.  We want to silently ignore
  those errors.  We currently just accept that in the non-error cases we
  will end up with duplicate (and likely unused) resources being created.
*/

template <typename E>
class is_ignorable_duplicate
{
public:
  static bool value(E e) { return false; }
};

#ifdef ANON_AWS_EC2
template <>
class is_ignorable_duplicate<Aws::EC2::EC2Errors>
{
public:
  static bool value(Aws::EC2::EC2Errors e)
  {
    auto ret = e == Aws::EC2::EC2Errors::ROUTE_ALREADY_EXISTS
      || e == Aws::EC2::EC2Errors::VPC_PEERING_CONNECTION_ALREADY_EXISTS
      || e == Aws::EC2::EC2Errors::NETWORK_ACL_ENTRY_ALREADY_EXISTS
      || e == Aws::EC2::EC2Errors::FLOW_LOG_ALREADY_EXISTS
      || e == Aws::EC2::EC2Errors::INVALID_SUBNET__CONFLICT
      || e == Aws::EC2::EC2Errors::INVALID_A_M_I_NAME__DUPLICATE
      || e == Aws::EC2::EC2Errors::INVALID_A_M_I_NAME__DUPLICATE
      || e == Aws::EC2::EC2Errors::INVALID_CUSTOMER_GATEWAY__DUPLICATE_IP_ADDRESS
      || e == Aws::EC2::EC2Errors::INVALID_GROUP__DUPLICATE
      || e == Aws::EC2::EC2Errors::INVALID_KEY_PAIR__DUPLICATE
      || e == Aws::EC2::EC2Errors::INVALID_PERMISSION__DUPLICATE
      || e == Aws::EC2::EC2Errors::INVALID_PERMISSION__DUPLICATE
      || e == Aws::EC2::EC2Errors::INVALID_PLACEMENT_GROUP__DUPLICATE;
    return ret;
  }
};
#endif

#ifdef ANON_AWS_DDB
template <>
class is_ignorable_duplicate<Aws::DynamoDB::DynamoDBErrors>
{
public:
  static bool value(Aws::DynamoDB::DynamoDBErrors e)
  {
    auto ret = e == Aws::DynamoDB::DynamoDBErrors::GLOBAL_TABLE_ALREADY_EXISTS
      || e == Aws::DynamoDB::DynamoDBErrors::REPLICA_ALREADY_EXISTS
      || e == Aws::DynamoDB::DynamoDBErrors::TABLE_ALREADY_EXISTS
      || e == Aws::DynamoDB::DynamoDBErrors::CONDITIONAL_CHECK_FAILED;
    return ret;
  }
};
#endif

// not all aws api sets require the "ignorable duplicate"
// handling logic on our side.  Those with "idempotency-token"
// handle the problem on their side.  Those that have this
// handling are:
//    route53
//    acm
//    globalaccelerator


#ifdef ANON_AWS_S3
template <>
class is_ignorable_duplicate<Aws::S3::S3Errors>
{
public:
  static bool value(Aws::S3::S3Errors e)
  {
    return e == Aws::S3::S3Errors::BUCKET_ALREADY_EXISTS
      || e == Aws::S3::S3Errors::BUCKET_ALREADY_OWNED_BY_YOU;
  }
};
#endif

#ifdef ANON_AWS_ELBV2
template <>
class is_ignorable_duplicate<Aws::ElasticLoadBalancingv2::ElasticLoadBalancingv2Errors>
{
public:
  static bool value(Aws::ElasticLoadBalancingv2::ElasticLoadBalancingv2Errors e)
  {
    return e == Aws::ElasticLoadBalancingv2::ElasticLoadBalancingv2Errors::DUPLICATE_LOAD_BALANCER_NAME;
  }
};
#endif

template <typename T>
bool aws_check_(
  const T &outcome,
  const std::function<void(std::ostream &formatter)> &fn)
{
  if (!outcome.IsSuccess())
  {
    auto &e = outcome.GetError();
    auto et = e.GetErrorType();
    if (generic_retry(et) || generic_retry_2(e))
      throw aws_throttle_error(Log::fmt(fn));
    if (is_ignorable_duplicate<decltype(et)>::value(et))
      return false;
    throw std::runtime_error(Log::fmt(fn));
  }
  return true;
}

#define aws_check(_outcome, _body) aws_check_(_outcome, [&](std::ostream &formatter) { formatter << _body << " failed: (" << (int)_outcome.GetError().GetErrorType() << ") " << _outcome.GetError(); })

void aws_throttle(const std::string &region, const std::function<void()> &fn);

#endif
