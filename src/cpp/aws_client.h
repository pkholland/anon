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

#pragma once

#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/client/RetryStrategy.h>
#include <aws/ec2/EC2Client.h>
#include <aws/dynamodb/DynamoDBClient.h>
#include <aws/route53/Route53Client.h>
#include <aws/s3/S3Client.h>
#include <aws/acm/ACMClient.h>
#include <aws/sqs/SQSClient.h>

void aws_client_init();
void aws_client_term();

// get a reasonable CredentialsProvider.  Of importance, if you are
// running in EC2 and have not specified an environment variable like AWS_PROFILE
// (which can redirect the provider to a specific case), then the credentials provider
// that this returns reads from EC2's bootstrap ip address to get the credentials
// (just as AWSCredentialsProviderChain would do) - but it does it in a fiber-safe
// way - which AWSCredentialsProviderChain would not be fiber safe.
const std::shared_ptr<Aws::Auth::AWSCredentialsProvider>& aws_get_cred_provider();

// returns the "default" aws region using the following rules:
//  a) if the environment variable AWS_DEFAULT_REGION is set it will be set to that value
//  b) else, if the environment variable AWS_PROFILE is set to a value that
//     matches an entry in your ~/.aws/config file, and that entry has a
//     region entry, it will be set to the value of that entry
//  c) else, if you have a ~/.aws/config file with an entry named "default"
//     and that entry has a region in it, it will be set to the value of that entry
//  c) else, if the application is running in EC2 it will be set to the
//     region that the EC2 instance is running in
//   d) else, it will be set to "us-east-1"

const std::string &aws_get_default_region();

// set client_cfg's:
//  1) region - set to the value passed
//  2) executor
//      this is set to an AWS Threading::Executor that runs all AWS tasks in fibers
//  3) retryStrategy
//      this is set to an implementation that is similar to AWS's DefaultRetryStrategy
//      except that it uses fiber sleeping functions instead of thread-sleeping functions
//      when it sleeps before retrying
void aws_init_client_config(Aws::Client::ClientConfiguration &client_cfg, const std::string &region);

std::shared_ptr<Aws::Client::RetryStrategy> aws_fiber_retry_strategy(long maxRetries = 10, long scaleFactor = 25);

// a cache of configs set up with aws_init_client_config
const Aws::Client::ClientConfiguration& aws_get_client_config(const std::string& region);

const Aws::EC2::EC2Client& aws_get_ec2_client(const std::string& region);
const Aws::DynamoDB::DynamoDBClient& aws_get_ddb_client(const std::string& region);
const Aws::Route53::Route53Client& aws_get_r53_client();
const Aws::S3::S3Client& aws_get_s3_client(const std::string& region);
const Aws::ACM::ACMClient& aws_get_acm_client(const std::string& region);
const Aws::SQS::SQSClient& aws_get_sqs_client(const std::string& region);


#endif
