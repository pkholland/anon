/*
 Copyright (c) 2018 ANON authors, see AUTHORS file.
 
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

#include "http_error.h"
#include "nlohmann/json.hpp"
#include <pcrecpp.h>
#include "request_dispatcher_priv.h"

/*
 * class to help in mapping url paths to handling function
 * 
 * There are two main types of handling functions supported here
 * - those that want to read the body of an http message as a json
 * objct, and those that do not (it is always possible for you to
 * read the body yourself in your function and do whatever you want,
 * but this class has helper logic for the case where you want to read
 * it as a json object).
 * 
 * Here is a trivial example of usage:
 * 
 *    // all paths will be under "/myapi"
 *    request_dispatcher rd("/myapi");
 *    
 *    rd.request_mapping(
 *        "GET",
 *        "hello",
 *        [](http_server::pipe_t &pipe, const http_request &request, bool is_tls){
 *          http_response response;
 *          response.add_header("content-type", "text/plain");
 *          response << "myapi says hello\n";
 *          pipe.respond(response);
 *        });
 * 
 * This example calls the function above whenever a client issues
 * an http GET to /myapi/hello.
 * 
 * request_dispatcher also supports url path variables, query
 * string, and http header value parsing - all by specifying
 * more sophisticated "path_spec" values (the parameter that is
 * simply "hello" in the example above).  Consider an api design
 * that puts a client name in the path url such that:
 * 
 *    /myapi/jeff/hello
 *    /myapi/sarah/hello
 *    /myapi/bob/hello
 * 
 * should all map to a "hello" function with the client name
 * passed to that function as an argument.  You do that like this:
 * 
 *    rd.request_mapping(
 *        "GET",
 *        "{client_name}/hello",
 *        [](http_server::pipe_t &pipe, const http_request &request, bool is_tls, const std::string& client_name){
 *          http_response response;
 *          response.add_header("content-type", "text/plain");
 *          response << "hi " << client_name << ", how's it going?\n";
 *          pipe.respond(response);
 *        });
 * 
 * where the basic rules are that path variables are specified
 * by "{...}" sequences in the path_spec, and require a matching
 * std::string argument to the function you supply.  The example
 * above seems to suggest that there is a relationship between the
 * name you supply inside the {...} and the name of the argument
 * to your function - but this isn't true.  It's probably a good
 * idea for you to do this, but the name inside the {...} and
 * the name of your function argument are ignored.  The only thing
 * that matters is the _order_ of these.  If you supply a path_spec
 * that looks like:
 * 
 *    "foo/{var1}/bar/{var2}/bonk/{var3}"
 * 
 * that will map to a function whose argument types are:
 * 
 *    (pipe, request, string, string, string)
 * 
 * and the value of the string arguments will be the values
 * of var1, var2, and var3 - in that order.  You are free to
 * name your functio arguments anything you want, as you can
 * the contents inside the {...}.  The above path_spec is
 * equivelent to:
 * 
 *    "foo/{}/bar/{}/bonk/{}"
 * 
 * One rule about the way you specify path variables is that
 * they have to be the entire contents between the "/"'s.
 * That is, you can't give a path spec that looks like:
 * 
 *    "foo/hello-{name}/bar"
 * 
 * and then provide a function whose arguments are:
 * 
 *    (pipe, request, string)
 * 
 * and use a GET request to "/myapi/foo/hello-bob/bar"
 * and try to capture just the "bob" of the url as your
 * string argument.  If you need to support urls like
 * this one, you need to specify the path spec as:
 * 
 *    "foo/{name}/bar"
 * 
 * and then remove the "hello-" part of the string yourself
 * when your function is called - it will be called with the
 * string "hello-bob".
 * 
 * Query strings
 * 
 * To get a request_dispatcher to parse the query string values
 * "accountName" and "password" for you, you use a path_spec that
 * looks like:
 * 
 *    "foo/{var1}/bar?accountName&password"
 * 
 * The "?" character in the path_spec separates the path part
 * of the spec from the query string part.  The query string
 * values will be passed to your function as additional string
 * arguments.  Like the path variables, these values will be in
 * the order you have specified them in the path_spec, with all
 * of the path variable arguments first, followed by all of the
 * query spec arguments.  The example above would be called as:
 * 
 *  (pipe, request, string var1, string, accountName, string password)
 * 
 * Query string values can either be required or optional.  By
 * default they are optional meaning that a request that did
 * not specify one of them would result in your function being
 * called with the corresponding argument set to "".  To make one
 * or more of them be required you put a "+" character at the
 * start.  So if accountName was required, but password was
 * optional you would specify it as:
 * 
 *    "foo/{var1}/bar?+accountName&password"
 * 
 * if a required query string value is not supplied in the url
 * then request_dispatcher will return a 400 response to the
 * caller and will not call your function.
 * 
 * Http Headers
 * 
 * Like query string values, request_dispatcher can parse and
 * pass header values to your function if you specify them in
 * your path_spec.  Header values follow the same syntax as
 * query string values and are specified after the second
 * "&" in the path spec.  They follow the same ordering rules
 * regarding which parameter is used when calling your function.
 * Here are some example path_specs and their treatment:
 * 
 *    "foo??+Authorization"
 *    calls
 *    (pipe, request, string auth)
 *    - returns 400 without calling the function if Authorization is missing
 * 
 *    "foo/{account}?password?+Authorization"
 *    calls
 *    (pipe, request, string acc, string pass, string auth)
 *    - returns 400 if Authorization is missing, calls with "" if password is missing
 * 
 * The non-variable portion of a path and qeury string values
 * are treated case sensitive.  Header values are case insensitive
 * 
 * That is, an http message that looks like:
 * 
 *    GET foo/myaccount?Password=hello HTTP/1.1
 *    authorization: letMeIn
 * 
 * will call the above function with the "pass" argument set
 * to "" because Password is not the same as password, and the
 * path_spec did not specify Password as required.
 * 
 * 
 *    GET foo/myaccount?password=hello HTTP/1.1
 *    authorization: letMeIn
 * 
 * will call the function with "pass" set to "hello", and "auth"
 * set to "letMeIn" because Authorization is treated as case
 * insensitive.
 * 
 * Support for reading message bodies as json
 * 
 * If your api design uses the body of the http message as
 * json, specifying other aspects of the api, you can use the
 * request_dispatcher::request_mapping_body method with all the
 * same rules as discussed above, except that you specify your
 * function with an additional argument at the end of type
 * const nlohmann::json&.  This support requries that the entire
 * body of the message be exactly one json object and that the
 * Content-Length of the header be set to this length.  Somewhat
 * like error condition handling above, if the body is not parsable
 * as a json object request_dispatcher will return 400 responses
 * to the caller without calling your function.
 * 
 * Some final notes.
 * 
 * request_dispatcher is intended to to be used in a teflon-like
 * design where the request_dispatcher object is created and
 * initialized during teflon's server_init call.  Dispatching
 * to your functions happens via you calling
 * request_dispatcher::dispatch, which is designed to be called
 * during your implementation of teflon's server_respond.  So
 * you end up with teflon-based code that looks roughly like:
 * 
 *    request_dispatcher  rd("/myapi");
 * 
 *    void server_init(...)
 *    {
 *      rd.request_mapping(1...);
 *      rd.request_mapping(2...);
 *      rd.request_mapping(3...);
 *      rd.request_mapping_body(4...);
 *      ...
 *    }
 * 
 *    void server_respond(http_server::pipe_t &pipe, const http_request &request, bool is_tls)
 *    {
 *      rd.dispatch(pipe, request, is_tls);
 *    }
 */
class request_dispatcher
{
  std::map<std::string, std::map<std::string, std::vector<std::function<bool(http_server::pipe_t &, const http_request &, bool, const std::string &, const std::string &, bool)>>>> _map;
  pcrecpp::RE _split_at_var;
  std::string _root_path;
  std::string _options;
  int _cors_enabled;

public:
  request_dispatcher(const std::string &root_path, int cors_enabled = 0)
      : _split_at_var("([^?{]*)(.*)"),
        _root_path(root_path),
        _options("OPTIONS"),
        _cors_enabled(cors_enabled)
  {
  }

  template <typename Fn>
  void request_mapping(const std::string &method, const std::string &path_spec, Fn f,
     const std::vector<std::string>& allowed_headers = std::vector<std::string>())
  {
    auto full_path_spec = _root_path + path_spec;
    std::string non_var, var;
    if (!_split_at_var.FullMatch(full_path_spec, &non_var, &var))
      anon_throw(std::runtime_error, "path split failed, invalid path: " << full_path_spec);
    _map[method][non_var].push_back(get_map_responder(f, allowed_headers, request_mapping_helper(full_path_spec)));
  }

  template <typename Fn>
  void request_mapping_body(const std::string &method, const std::string &path_spec, Fn f,
    const std::vector<std::string>& allowed_headers = std::vector<std::string>())
  {
    auto full_path_spec = _root_path + path_spec;
    std::string non_var, var;
    if (!_split_at_var.FullMatch(full_path_spec, &non_var, &var))
      anon_throw(std::runtime_error, "path split failed, invalid path: " << full_path_spec);
    _map[method][non_var].push_back(get_map_responder_body(f, allowed_headers, request_mapping_helper(full_path_spec)));
  }

  void dispatch(http_server::pipe_t &pipe, const http_request &request, bool is_tls);
};
