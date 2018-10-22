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

#pragma once

#include <aws/core/Aws.h>
#include <aws/dynamodb/DynamoDBClient.h>
#include <aws/dynamodb/model/GetItemRequest.h>
#include <aws/core/utils/Outcome.h>
#include "http_error.h"

class dynamoDB
{
  Aws::DynamoDB::DynamoDBClient _client;

public:
  dynamoDB(const std::shared_ptr<Aws::Auth::AWSCredentialsProvider> &provider,
           const Aws::Client::ClientConfiguration &client_config)
      : _client(provider, client_config)
  {
  }

  template <typename Fn>
  auto get_item(Fn deserializer, const std::string &table_name,
                const std::string &primary_key_name, const std::string &primary_key_value,
                const std::string &proj = "") -> decltype(deserializer(*(Aws::Map<Aws::String, Aws::DynamoDB::Model::AttributeValue> *)0))
  {
    Aws::DynamoDB::Model::GetItemRequest req;

    Aws::DynamoDB::Model::AttributeValue primary_key;
    primary_key.SetS(primary_key_value);
    req.WithTableName(table_name).AddKey(primary_key_name, primary_key);

    if (!proj.empty())
      req.SetProjectionExpression(proj);

    auto out = _client.GetItem(req);
    if (!out.IsSuccess())
    {
      auto &e = out.GetError();
      throw_request_error(e.GetResponseCode(), e.GetMessage());
    }

    auto items = out.GetResult().GetItem();
    return deserializer(items);
  }
};
