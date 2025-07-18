// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

// brpc - A framework to host and access services throughout Baidu.

// Date: Sun Jul 13 15:04:18 CST 2014

#include <sys/types.h>
#include <sys/socket.h>
#include <fstream>
#include <gtest/gtest.h>
#include <google/protobuf/descriptor.h>
#include "butil/time.h"
#include "butil/macros.h"
#include "butil/fd_guard.h"
#include "butil/files/scoped_file.h"
#include "brpc/socket.h"
#include "butil/object_pool.h"
#include "brpc/builtin/version_service.h"
#include "brpc/builtin/health_service.h"
#include "brpc/builtin/list_service.h"
#include "brpc/builtin/status_service.h"
#include "brpc/builtin/threads_service.h"
#include "brpc/builtin/vlog_service.h"
#include "brpc/builtin/index_service.h"        // IndexService
#include "brpc/builtin/connections_service.h"  // ConnectionsService
#include "brpc/builtin/flags_service.h"        // FlagsService
#include "brpc/builtin/vars_service.h"         // VarsService
#include "brpc/builtin/rpcz_service.h"         // RpczService
#include "brpc/builtin/dir_service.h"          // DirService
#include "brpc/builtin/pprof_service.h"        // PProfService
#include "brpc/builtin/bthreads_service.h"     // BthreadsService
#include "brpc/builtin/ids_service.h"          // IdsService
#include "brpc/builtin/sockets_service.h"      // SocketsService
#include "brpc/builtin/bad_method_service.h"
#include "brpc/server.h"
#include "brpc/restful.h"
#include "brpc/channel.h"
#include "brpc/socket_map.h"
#include "brpc/controller.h"
#include "brpc/compress.h"
#include "echo.pb.h"
#include "v1.pb.h"
#include "v2.pb.h"
#include "v3.pb.h"

int main(int argc, char* argv[]) {
    testing::InitGoogleTest(&argc, argv);
    GFLAGS_NAMESPACE::ParseCommandLineFlags(&argc, &argv, true);
    return RUN_ALL_TESTS();
}

namespace brpc {
DECLARE_bool(enable_threads_service);
DECLARE_bool(enable_dir_service);

namespace policy {
DECLARE_bool(use_http_error_code);

extern bool SerializeRpcMessage(const google::protobuf::Message& serializer,
                                Controller& cntl, ContentType content_type,
                                CompressType compress_type,
                                ChecksumType checksum_type, butil::IOBuf* buf);
extern bool DeserializeRpcMessage(const butil::IOBuf& deserializer,
                                  Controller& cntl, ContentType content_type,
                                  CompressType compress_type,
                                  ChecksumType checksum_type,
                                  google::protobuf::Message* message);
}

}

namespace {
void* RunClosure(void* arg) {
    google::protobuf::Closure* done = (google::protobuf::Closure*)arg;
    done->Run();
    return NULL;
}

bool g_verify_success = true;
const std::string g_unauthorized_error_text = "unauthorized";

class MyAuthenticator : public brpc::Authenticator {
public:
    MyAuthenticator() = default;
    ~MyAuthenticator() override = default;
    int GenerateCredential(std::string*) const override {
        return 0;
    }

    int VerifyCredential(const std::string&,
                         const butil::EndPoint&,
                         brpc::AuthContext*) const override {
        return g_verify_success ? 0 : -1;
    }

    std::string GetUnauthorizedErrorText() const override {
        return g_unauthorized_error_text;
    }
};

bool g_delete = false;
const std::string EXP_REQUEST = "hello";
const std::string EXP_RESPONSE = "world";
const std::string EXP_REQUEST_BASE64 = "aGVsbG8=";
const std::string EXP_USER_FIELD_KEY = "hello";
const std::string EXP_USER_FIELD_VALUE = "world";

class EchoServiceImpl : public test::EchoService {
public:
    EchoServiceImpl() : count(0) {}
    virtual ~EchoServiceImpl() { g_delete = true; }
    virtual void Echo(google::protobuf::RpcController* cntl_base,
                      const test::EchoRequest* request,
                      test::EchoResponse* response,
                      google::protobuf::Closure* done) {
        brpc::ClosureGuard done_guard(done);
        brpc::Controller* cntl = (brpc::Controller*)cntl_base;
        count.fetch_add(1, butil::memory_order_relaxed);
        EXPECT_EQ(EXP_REQUEST, request->message());
        response->set_message(EXP_RESPONSE);
        if (request->sleep_us() > 0) {
            LOG(INFO) << "Sleep " << request->sleep_us() << " us, protocol="
                      << cntl->request_protocol(); 
            bthread_usleep(request->sleep_us());
        } else {
            LOG(INFO) << "No sleep, protocol=" << cntl->request_protocol();
        }
        if (cntl->has_request_user_fields()) {
            ASSERT_TRUE(!cntl->request_user_fields()->empty());
            std::string* val = cntl->request_user_fields()->seek(EXP_USER_FIELD_KEY);
            ASSERT_TRUE(val != NULL);
            ASSERT_EQ(*val, EXP_USER_FIELD_VALUE);
            cntl->response_user_fields()->insert(EXP_USER_FIELD_KEY, EXP_USER_FIELD_VALUE);
        }
    }

    virtual void ComboEcho(google::protobuf::RpcController*,
                           const test::ComboRequest* request,
                           test::ComboResponse* response,
                           google::protobuf::Closure* done) {
        brpc::ClosureGuard done_guard(done);
        for (int i = 0; i < request->requests_size(); ++i) {
            response->add_responses()->set_message(request->requests(i).message());
        }
    }

    virtual void BytesEcho1(google::protobuf::RpcController*,
                            const test::BytesRequest* request,
                            test::BytesResponse* response,
                            google::protobuf::Closure* done) {
        brpc::ClosureGuard done_guard(done);
        EXPECT_EQ(EXP_REQUEST, request->databytes());
        response->set_databytes(request->databytes());
    }

    virtual void BytesEcho2(google::protobuf::RpcController*,
                            const test::BytesRequest* request,
                            test::BytesResponse* response,
                            google::protobuf::Closure* done) {
        brpc::ClosureGuard done_guard(done);
        EXPECT_EQ(EXP_REQUEST_BASE64, request->databytes());
        response->set_databytes(request->databytes());
    }

    butil::atomic<int64_t> count;
};

// An evil service that fakes its `ServiceDescriptor'
class EvilService : public test::EchoService {
public:
    explicit EvilService(const google::protobuf::ServiceDescriptor* sd)
            : _sd(sd) {}

    const google::protobuf::ServiceDescriptor* GetDescriptor() {
        return _sd;
    }

private:
    const google::protobuf::ServiceDescriptor* _sd;
};

class ServerTest : public ::testing::Test{
protected:
    ServerTest() {};
    virtual ~ServerTest(){};
    virtual void SetUp() {};
    virtual void TearDown() {};

    void TestAddBuiltinService(
        const google::protobuf::ServiceDescriptor* conflict_sd) {
        brpc::Server server;
        EvilService evil(conflict_sd);
        EXPECT_EQ(0, server.AddServiceInternal(
                      &evil, false, brpc::ServiceOptions()));
        EXPECT_EQ(-1, server.AddBuiltinServices());
    }
};

TEST_F(ServerTest, sanity) {
    {
        brpc::Server server;
        ASSERT_EQ(-1, server.Start("127.0.0.1:12345:asdf", NULL));
        ASSERT_EQ(-1, server.Start("127.0.0.1:99999", NULL)); 
        ASSERT_EQ(0, server.Start("127.0.0.1:8613", NULL));
    }
    {
        brpc::Server server;
        // accept hostname as well.
        ASSERT_EQ(0, server.Start("localhost:8613", NULL));
    }
    {
        brpc::Server server;
        ASSERT_EQ(0, server.Start("localhost:0", NULL));
        // port should be replaced with the actually used one.
        ASSERT_NE(0, server.listen_address().port);
    }

    {
        brpc::Server server;
        ASSERT_EQ(-1, server.Start(99999, NULL));
        ASSERT_EQ(0, server.Start(8613, NULL));
    }
    {
        brpc::Server server;
        brpc::ServerOptions options;
        options.internal_port = 8613;          // The same as service port
        ASSERT_EQ(-1, server.Start("127.0.0.1:8613", &options));
        ASSERT_FALSE(server.IsRunning());      // Revert server's status
        // And release the listen port
        ASSERT_EQ(0, server.Start("127.0.0.1:8613", NULL));
    }
    {
        brpc::Server server;
        brpc::ServerOptions options;
        ASSERT_EQ(0, server.Start(brpc::PortRange(8000, 9000), &options));
        ASSERT_TRUE(server.IsRunning());
        ASSERT_EQ(0ul, server.service_count());
        ASSERT_TRUE(NULL == server.first_service());
        ASSERT_EQ(0, server.Stop(0));
        ASSERT_EQ(0, server.Join());
    }

    butil::EndPoint ep;
    MyAuthenticator auth;
    brpc::Server server;
    ASSERT_EQ(0, str2endpoint("127.0.0.1:8613", &ep));
    brpc::ServerOptions opt;
    opt.auth = &auth;
    ASSERT_EQ(0, server.Start(ep, &opt));
    ASSERT_TRUE(server.IsRunning());
    ASSERT_EQ(&auth, server.options().auth);
    ASSERT_EQ(0ul, server.service_count());
    ASSERT_TRUE(NULL == server.first_service());

    std::vector<google::protobuf::Service*> services;
    server.ListServices(&services);
    ASSERT_TRUE(services.empty());
    ASSERT_EQ(0UL, server.service_count());
    for (brpc::Server::ServiceMap::const_iterator it
                 = server._service_map.begin();
         it != server._service_map.end(); ++it) {
        ASSERT_TRUE(it->second.is_builtin_service);
    }

    ASSERT_EQ(0, server.Stop(0));
    ASSERT_EQ(0, server.Join());
}

TEST_F(ServerTest, invalid_protocol_in_enabled_protocols) {
    butil::EndPoint ep;
    ASSERT_EQ(0, str2endpoint("127.0.0.1:8613", &ep));
    brpc::Server server;
    brpc::ServerOptions opt;
    opt.enabled_protocols = "hehe baidu_std";
    ASSERT_EQ(-1, server.Start(ep, &opt));
}

class EchoServiceV1 : public v1::EchoService {
public:
    EchoServiceV1() : ncalled(0)
                    , ncalled_echo2(0)
                    , ncalled_echo3(0)
                    , ncalled_echo4(0)
                    , ncalled_echo5(0)
    {}
    virtual ~EchoServiceV1() {}
    virtual void Echo(google::protobuf::RpcController* cntl_base,
                      const v1::EchoRequest* request,
                      v1::EchoResponse* response,
                      google::protobuf::Closure* done) {
        brpc::Controller* cntl = static_cast<brpc::Controller*>(cntl_base);
        brpc::ClosureGuard done_guard(done);
        if (request->has_message()) {
            response->set_message(request->message() + "_v1");
        } else {
            CHECK_EQ(brpc::PROTOCOL_HTTP, cntl->request_protocol());
            cntl->response_attachment() = cntl->request_attachment();
        }
        ncalled.fetch_add(1);
    }
    virtual void Echo2(google::protobuf::RpcController*,
                      const v1::EchoRequest* request,
                      v1::EchoResponse* response,
                      google::protobuf::Closure* done) {
        brpc::ClosureGuard done_guard(done);
        response->set_message(request->message() + "_v1_Echo2");
        ncalled_echo2.fetch_add(1);
    }
    virtual void Echo3(google::protobuf::RpcController*,
                      const v1::EchoRequest* request,
                      v1::EchoResponse* response,
                      google::protobuf::Closure* done) {
        brpc::ClosureGuard done_guard(done);
        response->set_message(request->message() + "_v1_Echo3");
        ncalled_echo3.fetch_add(1);
    }
    virtual void Echo4(google::protobuf::RpcController*,
                      const v1::EchoRequest* request,
                      v1::EchoResponse* response,
                      google::protobuf::Closure* done) {
        brpc::ClosureGuard done_guard(done);
        response->set_message(request->message() + "_v1_Echo4");
        ncalled_echo4.fetch_add(1);
    }
    virtual void Echo5(google::protobuf::RpcController*,
                      const v1::EchoRequest* request,
                      v1::EchoResponse* response,
                      google::protobuf::Closure* done) {
        brpc::ClosureGuard done_guard(done);
        response->set_message(request->message() + "_v1_Echo5");
        ncalled_echo5.fetch_add(1);
    }
    
    butil::atomic<int> ncalled;
    butil::atomic<int> ncalled_echo2;
    butil::atomic<int> ncalled_echo3;
    butil::atomic<int> ncalled_echo4;
    butil::atomic<int> ncalled_echo5;
};

class EchoServiceV2 : public v2::EchoService {
public:
    EchoServiceV2() : ncalled(0) {}
    virtual ~EchoServiceV2() {}
    virtual void Echo(google::protobuf::RpcController*,
                      const v2::EchoRequest* request,
                      v2::EchoResponse* response,
                      google::protobuf::Closure* done) {
        brpc::ClosureGuard done_guard(done);
        response->set_value(request->value() + 1);
        ncalled.fetch_add(1);
    }
    butil::atomic<int> ncalled;
};

class EchoServiceV3 : public v3::EchoService {
public:
    void Echo(::google::protobuf::RpcController*,
              const v3::EchoRequest* request,
              v3::EchoResponse* response,
              ::google::protobuf::Closure* done) override {
        brpc::ClosureGuard done_guard(done);
        ASSERT_EQ(request->message(), EXP_REQUEST);
        response->set_message(EXP_RESPONSE);
    }
};

TEST_F(ServerTest, empty_enabled_protocols) {
    butil::EndPoint ep;
    ASSERT_EQ(0, str2endpoint("127.0.0.1:8613", &ep));
    brpc::Server server;
    EchoServiceImpl echo_svc;
    ASSERT_EQ(0, server.AddService(
                  &echo_svc, brpc::SERVER_DOESNT_OWN_SERVICE));
    brpc::ServerOptions opt;
    opt.enabled_protocols = "   ";
    ASSERT_EQ(0, server.Start(ep, &opt));

    brpc::Channel chan;
    brpc::ChannelOptions copt;
    copt.protocol = "baidu_std";
    ASSERT_EQ(0, chan.Init(ep, &copt));
    brpc::Controller cntl;
    test::EchoRequest req;
    test::EchoResponse res;
    req.set_message(EXP_REQUEST);
    test::EchoService_Stub stub(&chan);
    stub.Echo(&cntl, &req, &res, NULL);
    ASSERT_FALSE(cntl.Failed()) << cntl.ErrorText();

    ASSERT_EQ(0, server.Stop(0));
    ASSERT_EQ(0, server.Join());
}

TEST_F(ServerTest, only_allow_protocols_in_enabled_protocols) {
    butil::EndPoint ep;
    ASSERT_EQ(0, str2endpoint("127.0.0.1:8613", &ep));
    brpc::Server server;
    EchoServiceImpl echo_svc;
    ASSERT_EQ(0, server.AddService(
                  &echo_svc, brpc::SERVER_DOESNT_OWN_SERVICE));
    brpc::ServerOptions opt;
    opt.enabled_protocols = "hulu_pbrpc";
    ASSERT_EQ(0, server.Start(ep, &opt));

    brpc::ChannelOptions copt;
    brpc::Controller cntl;

    // http is always allowed.
    brpc::Channel http_channel;
    copt.protocol = "http";
    ASSERT_EQ(0, http_channel.Init(ep, &copt));
    cntl.Reset();
    http_channel.CallMethod(NULL, &cntl, NULL, NULL, NULL);
    ASSERT_FALSE(cntl.Failed()) << cntl.ErrorText() << cntl.response_attachment();

    // Unmatched protocols are not allowed.
    brpc::Channel chan;
    copt.protocol = "baidu_std";
    ASSERT_EQ(0, chan.Init(ep, &copt));
    test::EchoRequest req;
    test::EchoResponse res;
    cntl.Reset();
    req.set_message(EXP_REQUEST);
    test::EchoService_Stub stub(&chan);
    stub.Echo(&cntl, &req, &res, NULL);
    ASSERT_TRUE(cntl.Failed());
    ASSERT_TRUE(cntl.ErrorText().find("Got EOF of ") != std::string::npos);
    
    ASSERT_EQ(0, server.Stop(0));
    ASSERT_EQ(0, server.Join());
}

TEST_F(ServerTest, services_in_different_ns) {
    const int port = 9200;
    brpc::Server server1;
    EchoServiceV1 service_v1;
    ASSERT_EQ(0, server1.AddService(&service_v1, brpc::SERVER_DOESNT_OWN_SERVICE));
    ASSERT_EQ(0, server1.Start(port, NULL));
    brpc::Channel http_channel;
    brpc::ChannelOptions chan_options;
    chan_options.protocol = "http";
    ASSERT_EQ(0, http_channel.Init("0.0.0.0", port, &chan_options));
    brpc::Controller cntl;
    cntl.http_request().uri() = "/EchoService/Echo";
    cntl.http_request().set_method(brpc::HTTP_METHOD_POST);
    cntl.request_attachment().append("{\"message\":\"foo\"}");
    http_channel.CallMethod(NULL, &cntl, NULL, NULL, NULL);
    ASSERT_FALSE(cntl.Failed()) << cntl.ErrorText() << cntl.response_attachment();
    ASSERT_EQ(1, service_v1.ncalled.load());
    cntl.Reset();
    cntl.http_request().uri() = "/v1.EchoService/Echo";
    cntl.http_request().set_method(brpc::HTTP_METHOD_POST);
    cntl.request_attachment().append("{\"message\":\"foo\"}");
    http_channel.CallMethod(NULL, &cntl, NULL, NULL, NULL);
    ASSERT_FALSE(cntl.Failed()) << cntl.ErrorText() << cntl.response_attachment();
    ASSERT_EQ(2, service_v1.ncalled.load());
    //Stop the server to add another service.
    server1.Stop(0);
    server1.Join();
    // NOTICE: stopping server now does not trigger HC of the client because
    // the main socket is only SetFailed in RPC route, however the RPC already 
    // ends at this point.
    EchoServiceV2 service_v2;
#ifndef ALLOW_SAME_NAMED_SERVICE_IN_DIFFERENT_NAMESPACE
    ASSERT_EQ(-1, server1.AddService(&service_v2, brpc::SERVER_DOESNT_OWN_SERVICE));
#else
    ASSERT_EQ(0, server1.AddService(&service_v2, brpc::SERVER_DOESNT_OWN_SERVICE));
    ASSERT_EQ(0, server1.Start(port, NULL));
    //sleep(3); // wait for HC
    cntl.Reset();
    cntl.http_request().uri() = "/v2.EchoService/Echo";
    cntl.http_request().set_method(brpc::HTTP_METHOD_POST);
    cntl.request_attachment().append("{\"value\":33}");
    http_channel.CallMethod(NULL, &cntl, NULL, NULL, NULL);
    ASSERT_FALSE(cntl.Failed()) << cntl.ErrorText() << cntl.response_attachment();
    ASSERT_EQ(1, service_v2.ncalled.load());
    cntl.Reset();
    cntl.http_request().uri() = "/EchoService/Echo";
    cntl.http_request().set_method(brpc::HTTP_METHOD_POST);
    cntl.request_attachment().append("{\"value\":33}");
    http_channel.CallMethod(NULL, &cntl, NULL, NULL, NULL);
    ASSERT_FALSE(cntl.Failed()) << cntl.ErrorText() << cntl.response_attachment();
    ASSERT_EQ(2, service_v2.ncalled.load());
    server1.Stop(0);
    server1.Join();
#endif
}

TEST_F(ServerTest, various_forms_of_uri_paths) {
    const int port = 9200;
    brpc::Server server1;
    EchoServiceV1 service_v1;
    ASSERT_EQ(0, server1.AddService(&service_v1, brpc::SERVER_DOESNT_OWN_SERVICE));
    ASSERT_EQ(0, server1.Start(port, NULL));
    brpc::Channel http_channel;
    brpc::ChannelOptions chan_options;
    chan_options.protocol = "http";
    ASSERT_EQ(0, http_channel.Init("0.0.0.0", port, &chan_options));
    brpc::Controller cntl;
    cntl.http_request().uri() = "/EchoService/Echo";
    cntl.http_request().set_method(brpc::HTTP_METHOD_POST);
    cntl.request_attachment().append("{\"message\":\"foo\"}");
    http_channel.CallMethod(NULL, &cntl, NULL, NULL, NULL);
    ASSERT_FALSE(cntl.Failed()) << cntl.ErrorText() << cntl.response_attachment();
    ASSERT_EQ(1, service_v1.ncalled.load());
    
    cntl.Reset();
    cntl.http_request().uri() = "/EchoService///Echo//";
    cntl.http_request().set_method(brpc::HTTP_METHOD_POST);
    cntl.request_attachment().append("{\"message\":\"foo\"}");
    http_channel.CallMethod(NULL, &cntl, NULL, NULL, NULL);
    ASSERT_FALSE(cntl.Failed()) << cntl.ErrorText() << cntl.response_attachment();
    ASSERT_EQ(2, service_v1.ncalled.load());

    cntl.Reset();
    cntl.http_request().uri() = "/EchoService /Echo/";
    cntl.http_request().set_method(brpc::HTTP_METHOD_POST);
    cntl.request_attachment().append("{\"message\":\"foo\"}");
    http_channel.CallMethod(NULL, &cntl, NULL, NULL, NULL);
    ASSERT_TRUE(cntl.Failed());
    ASSERT_EQ(brpc::EREQUEST, cntl.ErrorCode());
    LOG(INFO) << "Expected error: " << cntl.ErrorText();
    ASSERT_EQ(2, service_v1.ncalled.load());

    // Additional path(stored in unresolved_path) after method is acceptible
    cntl.Reset();
    cntl.http_request().uri() = "/EchoService/Echo/Foo";
    cntl.http_request().set_method(brpc::HTTP_METHOD_POST);
    cntl.request_attachment().append("{\"message\":\"foo\"}");
    http_channel.CallMethod(NULL, &cntl, NULL, NULL, NULL);
    ASSERT_FALSE(cntl.Failed()) << cntl.ErrorText();
    ASSERT_EQ(3, service_v1.ncalled.load());
    
    //Stop the server.
    server1.Stop(0);
    server1.Join();
}

TEST_F(ServerTest, missing_required_fields) {
    const int port = 9200;
    brpc::Server server1;
    EchoServiceV1 service_v1;
    ASSERT_EQ(0, server1.AddService(&service_v1, brpc::SERVER_DOESNT_OWN_SERVICE));
    ASSERT_EQ(0, server1.Start(port, NULL));
    brpc::Channel http_channel;
    brpc::ChannelOptions chan_options;
    chan_options.protocol = "http";
    ASSERT_EQ(0, http_channel.Init("0.0.0.0", port, &chan_options));
    brpc::Controller cntl;
    cntl.http_request().uri() = "/EchoService/Echo";
    http_channel.CallMethod(NULL, &cntl, NULL, NULL, NULL);
    ASSERT_TRUE(cntl.Failed());
    ASSERT_EQ(brpc::EHTTP, cntl.ErrorCode());
    LOG(INFO) << cntl.ErrorText();
    ASSERT_EQ(brpc::HTTP_STATUS_BAD_REQUEST, cntl.http_response().status_code());
    ASSERT_EQ(0, service_v1.ncalled.load());

    cntl.Reset();
    cntl.http_request().uri() = "/EchoService/Echo";
    cntl.http_request().set_method(brpc::HTTP_METHOD_POST);
    http_channel.CallMethod(NULL, &cntl, NULL, NULL, NULL);
    ASSERT_TRUE(cntl.Failed());
    ASSERT_EQ(brpc::EHTTP, cntl.ErrorCode());
    ASSERT_EQ(brpc::HTTP_STATUS_BAD_REQUEST, cntl.http_response().status_code());
    ASSERT_EQ(0, service_v1.ncalled.load());

    cntl.Reset();
    cntl.http_request().uri() = "/EchoService/Echo";
    cntl.http_request().set_method(brpc::HTTP_METHOD_POST);
    cntl.request_attachment().append("{\"message2\":\"foo\"}");
    http_channel.CallMethod(NULL, &cntl, NULL, NULL, NULL);
    ASSERT_TRUE(cntl.Failed());
    ASSERT_EQ(brpc::EHTTP, cntl.ErrorCode());
    ASSERT_EQ(brpc::HTTP_STATUS_BAD_REQUEST, cntl.http_response().status_code());
    ASSERT_EQ(0, service_v1.ncalled.load());
}

TEST_F(ServerTest, disallow_http_body_to_pb) {
    const int port = 9200;
    brpc::Server server1;
    EchoServiceV1 service_v1;
    brpc::ServiceOptions svc_opt;
    svc_opt.allow_http_body_to_pb = false;
    svc_opt.restful_mappings = "/access_echo1=>Echo";
    ASSERT_EQ(0, server1.AddService(&service_v1, svc_opt));
    ASSERT_EQ(0, server1.Start(port, NULL));
    brpc::Channel http_channel;
    brpc::ChannelOptions chan_options;
    chan_options.protocol = "http";
    ASSERT_EQ(0, http_channel.Init("0.0.0.0", port, &chan_options));
    brpc::Controller cntl;
    cntl.http_request().uri() = "/access_echo1";
    http_channel.CallMethod(NULL, &cntl, NULL, NULL, NULL);
    ASSERT_TRUE(cntl.Failed());
    ASSERT_EQ(brpc::EHTTP, cntl.ErrorCode());
    ASSERT_EQ(brpc::HTTP_STATUS_INTERNAL_SERVER_ERROR,
              cntl.http_response().status_code());
    ASSERT_EQ(1, service_v1.ncalled.load());

    cntl.Reset();
    cntl.http_request().uri() = "/access_echo1";
    cntl.http_request().set_method(brpc::HTTP_METHOD_POST);
    cntl.request_attachment().append("heheda");
    http_channel.CallMethod(NULL, &cntl, NULL, NULL, NULL);
    ASSERT_FALSE(cntl.Failed()) << cntl.ErrorText();
    ASSERT_EQ("heheda", cntl.response_attachment());
    ASSERT_EQ(2, service_v1.ncalled.load());
}

TEST_F(ServerTest, restful_mapping) {
    const int port = 9200;
    EchoServiceV1 service_v1;
    EchoServiceV2 service_v2;
    
    brpc::Server server1;
    ASSERT_EQ(0u, server1.service_count());
    ASSERT_EQ(0, server1.AddService(
                  &service_v1,
                  brpc::SERVER_DOESNT_OWN_SERVICE,
                  "/v1/echo/ => Echo,"

                  // Map another path to the same method is ok.
                  "/v3/echo => Echo,"

                  // end with wildcard 
                  "/v2/echo/* => Echo,"

                  // single-component path should be OK
                  "/v4_echo => Echo,"

                  // heading slash can be ignored
                  " v5/echo => Echo,"

                  // with or without wildcard can coexist.
                  " /v6/echo => Echo,"
                  " /v6/echo/* => Echo2,"
                  " /v6/abc/*/def => Echo3,"
                  " /v6/echo/*.flv => Echo4,"
                  " /v6/*.flv => Echo5,"
                  " *.flv => Echo,"
                  ));
    ASSERT_EQ(1u, server1.service_count());
    ASSERT_TRUE(server1._global_restful_map);
    ASSERT_EQ(1UL, server1._global_restful_map->size());

    // Disallow duplicated path
    brpc::Server server2;
    ASSERT_EQ(-1, server2.AddService(
                  &service_v1,
                  brpc::SERVER_DOESNT_OWN_SERVICE,
                  "/v1/echo => Echo,"
                  "/v1/echo => Echo"));
    ASSERT_EQ(0u, server2.service_count());
    
    // NOTE: PATH/* and PATH cannot coexist in previous versions, now it's OK.
    brpc::Server server3;
    ASSERT_EQ(0, server3.AddService(
                  &service_v1,
                  brpc::SERVER_DOESNT_OWN_SERVICE,
                  "/v1/echo/* => Echo,"
                  "/v1/echo   => Echo"));
    ASSERT_EQ(1u, server3.service_count());
    
    // Same named services can't be added even with restful mapping
    brpc::Server server4;
    ASSERT_EQ(0, server4.AddService(
                  &service_v1,
                  brpc::SERVER_DOESNT_OWN_SERVICE,
                  "/v1/echo => Echo"));
    ASSERT_EQ(1u, server4.service_count());
    ASSERT_EQ(-1, server4.AddService(
                  &service_v2,
                  brpc::SERVER_DOESNT_OWN_SERVICE,
                  "/v2/echo => Echo"));
    ASSERT_EQ(1u, server4.service_count());

    // Invalid method name.
    brpc::Server server5;
    ASSERT_EQ(-1, server5.AddService(
                  &service_v1,
                  brpc::SERVER_DOESNT_OWN_SERVICE,
                  "/v1/echo => UnexistMethod"));
    ASSERT_EQ(0u, server5.service_count());

    // Invalid path.
    brpc::Server server6;
    ASSERT_EQ(-1, server6.AddService(
                  &service_v1,
                  brpc::SERVER_DOESNT_OWN_SERVICE,
                  "/v1/ echo => Echo"));
    ASSERT_EQ(0u, server6.service_count());

    // Empty path
    brpc::Server server7;
    ASSERT_EQ(-1, server7.AddService(
                  &service_v1,
                  brpc::SERVER_DOESNT_OWN_SERVICE,
                  "  => Echo"));
    ASSERT_EQ(0u, server7.service_count());

    // Disabled pattern "/A*/B => M"
    brpc::Server server8;
    ASSERT_EQ(-1, server8.AddService(
                  &service_v1,
                  brpc::SERVER_DOESNT_OWN_SERVICE,
                  " abc* => Echo"));
    ASSERT_EQ(0u, server8.service_count());
    ASSERT_EQ(-1, server8.AddService(
                  &service_v1,
                  brpc::SERVER_DOESNT_OWN_SERVICE,
                  " abc/def* => Echo"));
    ASSERT_EQ(0u, server8.service_count());

    // More than one wildcard
    brpc::Server server9;
    ASSERT_EQ(-1, server9.AddService(
                  &service_v1,
                  brpc::SERVER_DOESNT_OWN_SERVICE,
                  " /v1/*/* => Echo"));
    ASSERT_EQ(0u, server9.service_count());
    
    // default url access
    brpc::Server server10;
    ASSERT_EQ(0, server10.AddService(
                  &service_v1,
                  brpc::SERVER_DOESNT_OWN_SERVICE,
                  "/v1/echo => Echo",
                  true));
    ASSERT_EQ(1u, server10.service_count());
    ASSERT_FALSE(server10._global_restful_map);

    // Access services
    ASSERT_EQ(0, server1.Start(port, NULL));
    brpc::Channel http_channel;
    brpc::ChannelOptions chan_options;
    chan_options.protocol = "http";
    ASSERT_EQ(0, http_channel.Init("0.0.0.0", port, &chan_options));

    // reject /EchoService/Echo
    brpc::Controller cntl;
    cntl.http_request().uri() = "/EchoService/Echo";
    cntl.http_request().set_method(brpc::HTTP_METHOD_POST);
    cntl.request_attachment().append("{\"message\":\"foo\"}");
    http_channel.CallMethod(NULL, &cntl, NULL, NULL, NULL);
    ASSERT_TRUE(cntl.Failed());
    ASSERT_EQ(0, service_v1.ncalled.load());

    // access v1.Echo via /v1/echo.
    cntl.Reset();
    cntl.http_request().uri() = "/v1/echo";
    cntl.http_request().set_method(brpc::HTTP_METHOD_POST);
    cntl.request_attachment().append("{\"message\":\"foo\"}");
    http_channel.CallMethod(NULL, &cntl, NULL, NULL, NULL);
    ASSERT_FALSE(cntl.Failed()) << cntl.ErrorText();
    ASSERT_EQ(1, service_v1.ncalled.load());
    ASSERT_EQ("{\"message\":\"foo_v1\"}", cntl.response_attachment());

    // access v1.Echo via /v3/echo.
    cntl.Reset();
    cntl.http_request().uri() = "/v3/echo";
    cntl.http_request().set_method(brpc::HTTP_METHOD_POST);
    cntl.request_attachment().append("{\"message\":\"bar\"}");
    http_channel.CallMethod(NULL, &cntl, NULL, NULL, NULL);
    ASSERT_FALSE(cntl.Failed()) << cntl.ErrorText();
    ASSERT_EQ(2, service_v1.ncalled.load());
    ASSERT_EQ("{\"message\":\"bar_v1\"}", cntl.response_attachment());

    // Adding extra slashes (and heading/trailing spaces) is OK.
    cntl.Reset();
    cntl.http_request().uri() = " //v1///echo////  ";
    cntl.http_request().set_method(brpc::HTTP_METHOD_POST);
    cntl.request_attachment().append("{\"message\":\"hello\"}");
    http_channel.CallMethod(NULL, &cntl, NULL, NULL, NULL);
    ASSERT_FALSE(cntl.Failed()) << cntl.ErrorText();
    ASSERT_EQ(3, service_v1.ncalled.load());
    ASSERT_EQ("{\"message\":\"hello_v1\"}", cntl.response_attachment());

    // /v3/echo must be exactly matched.
    cntl.Reset();
    cntl.http_request().uri() = "/v3/echo/anything";
    cntl.http_request().set_method(brpc::HTTP_METHOD_POST);
    cntl.request_attachment().append("{\"message\":\"foo\"}");
    http_channel.CallMethod(NULL, &cntl, NULL, NULL, NULL);
    ASSERT_TRUE(cntl.Failed());
    ASSERT_EQ(brpc::EHTTP, cntl.ErrorCode());
    LOG(INFO) << "Expected error: " << cntl.ErrorText();
    ASSERT_EQ(3, service_v1.ncalled.load());

    // Access v1.Echo via /v2/echo
    cntl.Reset();
    cntl.http_request().uri() = "/v2/echo";
    cntl.http_request().set_method(brpc::HTTP_METHOD_POST);
    cntl.request_attachment().append("{\"message\":\"hehe\"}");
    http_channel.CallMethod(NULL, &cntl, NULL, NULL, NULL);
    ASSERT_FALSE(cntl.Failed()) << cntl.ErrorText();
    ASSERT_EQ(4, service_v1.ncalled.load());
    ASSERT_EQ("{\"message\":\"hehe_v1\"}", cntl.response_attachment());
    
    // Access v1.Echo via /v2/echo/anything
    cntl.Reset();
    cntl.http_request().uri() = "/v2/echo/anything";
    cntl.http_request().set_method(brpc::HTTP_METHOD_POST);
    cntl.request_attachment().append("{\"message\":\"good\"}");
    http_channel.CallMethod(NULL, &cntl, NULL, NULL, NULL);
    ASSERT_FALSE(cntl.Failed()) << cntl.ErrorText();
    ASSERT_EQ(5, service_v1.ncalled.load());
    ASSERT_EQ("{\"message\":\"good_v1\"}", cntl.response_attachment());

    cntl.Reset();
    cntl.http_request().uri() = "/v4_echo";
    cntl.http_request().set_method(brpc::HTTP_METHOD_POST);
    cntl.request_attachment().append("{\"message\":\"hoho\"}");
    http_channel.CallMethod(NULL, &cntl, NULL, NULL, NULL);
    ASSERT_FALSE(cntl.Failed()) << cntl.ErrorText();
    ASSERT_EQ(6, service_v1.ncalled.load());
    ASSERT_EQ("{\"message\":\"hoho_v1\"}", cntl.response_attachment());

    cntl.Reset();
    cntl.http_request().uri() = "/v5/echo";
    cntl.http_request().set_method(brpc::HTTP_METHOD_POST);
    cntl.request_attachment().append("{\"message\":\"xyz\"}");
    http_channel.CallMethod(NULL, &cntl, NULL, NULL, NULL);
    ASSERT_FALSE(cntl.Failed()) << cntl.ErrorText();
    ASSERT_EQ(7, service_v1.ncalled.load());
    ASSERT_EQ("{\"message\":\"xyz_v1\"}", cntl.response_attachment());

    cntl.Reset();
    cntl.http_request().uri() = "/v6/echo";
    cntl.http_request().set_method(brpc::HTTP_METHOD_POST);
    cntl.request_attachment().append("{\"message\":\"xyz\"}");
    http_channel.CallMethod(NULL, &cntl, NULL, NULL, NULL);
    ASSERT_FALSE(cntl.Failed()) << cntl.ErrorText();
    ASSERT_EQ(8, service_v1.ncalled.load());
    ASSERT_EQ("{\"message\":\"xyz_v1\"}", cntl.response_attachment());
    
    cntl.Reset();
    cntl.http_request().uri() = "/v6/echo/test";
    cntl.http_request().set_method(brpc::HTTP_METHOD_POST);
    cntl.request_attachment().append("{\"message\":\"xyz\"}");
    http_channel.CallMethod(NULL, &cntl, NULL, NULL, NULL);
    ASSERT_FALSE(cntl.Failed()) << cntl.ErrorText();
    ASSERT_EQ(1, service_v1.ncalled_echo2.load());
    ASSERT_EQ("{\"message\":\"xyz_v1_Echo2\"}", cntl.response_attachment());

    cntl.Reset();
    cntl.http_request().uri() = "/v6/abc/heheda/def";
    cntl.http_request().set_method(brpc::HTTP_METHOD_POST);
    cntl.request_attachment().append("{\"message\":\"abc_heheda\"}");
    http_channel.CallMethod(NULL, &cntl, NULL, NULL, NULL);
    ASSERT_FALSE(cntl.Failed()) << cntl.ErrorText();
    ASSERT_EQ(1, service_v1.ncalled_echo3.load());
    ASSERT_EQ("{\"message\":\"abc_heheda_v1_Echo3\"}", cntl.response_attachment());

    cntl.Reset();
    cntl.http_request().uri() = "/v6/abc/def";
    cntl.http_request().set_method(brpc::HTTP_METHOD_POST);
    cntl.request_attachment().append("{\"message\":\"abc\"}");
    http_channel.CallMethod(NULL, &cntl, NULL, NULL, NULL);
    ASSERT_FALSE(cntl.Failed()) << cntl.ErrorText();
    ASSERT_EQ(2, service_v1.ncalled_echo3.load());
    ASSERT_EQ("{\"message\":\"abc_v1_Echo3\"}", cntl.response_attachment());

    // Incorrect suffix
    cntl.Reset();
    cntl.http_request().uri() = "/v6/abc/heheda/def2";
    cntl.http_request().set_method(brpc::HTTP_METHOD_POST);
    cntl.request_attachment().append("{\"message\":\"xyz\"}");
    http_channel.CallMethod(NULL, &cntl, NULL, NULL, NULL);
    ASSERT_TRUE(cntl.Failed());
    ASSERT_EQ(2, service_v1.ncalled_echo3.load());
    
    cntl.Reset();
    cntl.http_request().uri() = "/v6/echo/1.flv";
    cntl.http_request().set_method(brpc::HTTP_METHOD_POST);
    cntl.request_attachment().append("{\"message\":\"1.flv\"}");
    http_channel.CallMethod(NULL, &cntl, NULL, NULL, NULL);
    ASSERT_FALSE(cntl.Failed()) << cntl.ErrorText();
    ASSERT_EQ("{\"message\":\"1.flv_v1_Echo4\"}", cntl.response_attachment());
    ASSERT_EQ(1, service_v1.ncalled_echo4.load());

    cntl.Reset();
    cntl.http_request().uri() = "//v6//d.flv//";
    cntl.http_request().set_method(brpc::HTTP_METHOD_POST);
    cntl.request_attachment().append("{\"message\":\"d.flv\"}");
    http_channel.CallMethod(NULL, &cntl, NULL, NULL, NULL);
    ASSERT_FALSE(cntl.Failed()) << cntl.ErrorText();
    ASSERT_EQ("{\"message\":\"d.flv_v1_Echo5\"}", cntl.response_attachment());
    ASSERT_EQ(1, service_v1.ncalled_echo5.load());

    // matched the global restful map.
    cntl.Reset();
    cntl.http_request().uri() = "//d.flv//";
    cntl.http_request().set_method(brpc::HTTP_METHOD_POST);
    cntl.request_attachment().append("{\"message\":\"d.flv\"}");
    http_channel.CallMethod(NULL, &cntl, NULL, NULL, NULL);
    ASSERT_FALSE(cntl.Failed()) << cntl.ErrorText();
    ASSERT_EQ("{\"message\":\"d.flv_v1\"}", cntl.response_attachment());
    ASSERT_EQ(9, service_v1.ncalled.load());

    cntl.Reset();
    cntl.http_request().uri() = "/v7/e.flv";
    cntl.http_request().set_method(brpc::HTTP_METHOD_POST);
    cntl.request_attachment().append("{\"message\":\"e.flv\"}");
    http_channel.CallMethod(NULL, &cntl, NULL, NULL, NULL);
    ASSERT_FALSE(cntl.Failed()) << cntl.ErrorText();
    ASSERT_EQ("{\"message\":\"e.flv_v1\"}", cntl.response_attachment());
    ASSERT_EQ(10, service_v1.ncalled.load());

    cntl.Reset();
    cntl.http_request().uri() = "/v0/f.flv";
    cntl.http_request().set_method(brpc::HTTP_METHOD_POST);
    cntl.request_attachment().append("{\"message\":\"f.flv\"}");
    http_channel.CallMethod(NULL, &cntl, NULL, NULL, NULL);
    ASSERT_FALSE(cntl.Failed()) << cntl.ErrorText();
    ASSERT_EQ("{\"message\":\"f.flv_v1\"}", cntl.response_attachment());
    ASSERT_EQ(11, service_v1.ncalled.load());
    
    // matched nothing
    cntl.Reset();
    cntl.http_request().uri() = "/v6/ech/1.ts";
    cntl.http_request().set_method(brpc::HTTP_METHOD_POST);
    cntl.request_attachment().append("{\"message\":\"1.ts\"}");
    http_channel.CallMethod(NULL, &cntl, NULL, NULL, NULL);
    ASSERT_TRUE(cntl.Failed());

    //Stop the server.
    server1.Stop(0);
    server1.Join();

    ASSERT_EQ(0, server10.Start(port, NULL));

    // access v1.Echo via /v1/echo.
    cntl.Reset();
    cntl.http_request().uri() = "/v1/echo";
    cntl.http_request().set_method(brpc::HTTP_METHOD_POST);
    cntl.request_attachment().append("{\"message\":\"foo\"}");
    http_channel.CallMethod(NULL, &cntl, NULL, NULL, NULL);
    ASSERT_FALSE(cntl.Failed()) << cntl.ErrorText();
    ASSERT_EQ(12, service_v1.ncalled.load());
    ASSERT_EQ("{\"message\":\"foo_v1\"}", cntl.response_attachment());

    // access v1.Echo via default url
    cntl.Reset();
    cntl.http_request().uri() = "/EchoService/Echo";
    cntl.http_request().set_method(brpc::HTTP_METHOD_POST);
    cntl.request_attachment().append("{\"message\":\"foo\"}");
    http_channel.CallMethod(NULL, &cntl, NULL, NULL, NULL);
    ASSERT_FALSE(cntl.Failed()) << cntl.ErrorText();
    ASSERT_EQ(13, service_v1.ncalled.load());
    ASSERT_EQ("{\"message\":\"foo_v1\"}", cntl.response_attachment());

    server10.Stop(0);
    server10.Join();

    // Removing the service should update _global_restful_map.
    ASSERT_EQ(0, server1.RemoveService(&service_v1));
    ASSERT_EQ(0u, server1.service_count());
    ASSERT_TRUE(server1._global_restful_map); // deleted in dtor.
    ASSERT_EQ(0u, server1._global_restful_map->size());
}

TEST_F(ServerTest, http_error_code) {
    brpc::policy::FLAGS_use_http_error_code = true;

    const int port = 9200;
    // missing_required_fields -> brpc::EREQUEST
    {
        brpc::Server server1;
        EchoServiceV1 service_v1;
        ASSERT_EQ(0, server1.AddService(&service_v1, brpc::SERVER_DOESNT_OWN_SERVICE));
        ASSERT_EQ(0, server1.Start(port, NULL));

        brpc::Channel http_channel;
        brpc::ChannelOptions chan_options;
        chan_options.protocol = "http";
        ASSERT_EQ(0, http_channel.Init("0.0.0.0", port, &chan_options));
        brpc::Controller cntl;
        cntl.http_request().uri() = "/EchoService/Echo";
        http_channel.CallMethod(NULL, &cntl, NULL, NULL, NULL);
        ASSERT_TRUE(cntl.Failed());
        ASSERT_EQ(brpc::EREQUEST, cntl.ErrorCode());
        LOG(INFO) << cntl.ErrorText();
        ASSERT_EQ(brpc::HTTP_STATUS_BAD_REQUEST, cntl.http_response().status_code());
        ASSERT_EQ(0, service_v1.ncalled.load());
    }

    // disallow_http_body_to_pb -> brpc::ERESPONSE
    {
        brpc::Server server1;
        EchoServiceV1 service_v1;
        brpc::ServiceOptions svc_opt;
        svc_opt.allow_http_body_to_pb = false;
        svc_opt.restful_mappings = "/access_echo1=>Echo";
        ASSERT_EQ(0, server1.AddService(&service_v1, svc_opt));
        ASSERT_EQ(0, server1.Start(port, NULL));
        brpc::Channel http_channel;
        brpc::ChannelOptions chan_options;
        chan_options.protocol = "http";
        ASSERT_EQ(0, http_channel.Init("0.0.0.0", port, &chan_options));
        brpc::Controller cntl;
        cntl.http_request().uri() = "/access_echo1";
        http_channel.CallMethod(NULL, &cntl, NULL, NULL, NULL);
        ASSERT_TRUE(cntl.Failed());
        ASSERT_EQ(brpc::ERESPONSE, cntl.ErrorCode());
        ASSERT_EQ(brpc::HTTP_STATUS_INTERNAL_SERVER_ERROR,
            cntl.http_response().status_code());
        ASSERT_EQ(1, service_v1.ncalled.load());
    }

    // restful_mapping -> brpc::ENOMETHOD
    {
        brpc::Server server1;
        EchoServiceV1 service_v1;
        ASSERT_EQ(0u, server1.service_count());
        ASSERT_EQ(0, server1.AddService(
            &service_v1,
            brpc::SERVER_DOESNT_OWN_SERVICE,
            "/v1/echo/ => Echo,"

            // Map another path to the same method is ok.
            "/v3/echo => Echo,"

            // end with wildcard
            "/v2/echo/* => Echo,"

            // single-component path should be OK
            "/v4_echo => Echo,"

            // heading slash can be ignored
            " v5/echo => Echo,"

            // with or without wildcard can coexist.
            " /v6/echo => Echo,"
            " /v6/echo/* => Echo2,"
            " /v6/abc/*/def => Echo3,"
            " /v6/echo/*.flv => Echo4,"
            " /v6/*.flv => Echo5,"
            " *.flv => Echo,"
        ));
        ASSERT_EQ(1u, server1.service_count());
        ASSERT_TRUE(server1._global_restful_map);
        ASSERT_EQ(1UL, server1._global_restful_map->size());

        ASSERT_EQ(0, server1.Start(port, NULL));
        brpc::Channel http_channel;
        brpc::ChannelOptions chan_options;
        chan_options.protocol = "http";
        ASSERT_EQ(0, http_channel.Init("0.0.0.0", port, &chan_options));
        brpc::Controller cntl;
        cntl.http_request().uri() = "/v3/echo/anything";
        cntl.http_request().set_method(brpc::HTTP_METHOD_POST);
        cntl.request_attachment().append("{\"message\":\"foo\"}");
        http_channel.CallMethod(NULL, &cntl, NULL, NULL, NULL);
        ASSERT_TRUE(cntl.Failed());
        ASSERT_EQ(brpc::ENOMETHOD, cntl.ErrorCode());
        LOG(INFO) << "Expected error: " << cntl.ErrorText();
        ASSERT_EQ(0, service_v1.ncalled.load());
    }

    // max_concurrency -> brpc::ELIMIT
    {
        brpc::Server server1;
        EchoServiceImpl service1;
        ASSERT_EQ(0, server1.AddService(&service1, brpc::SERVER_DOESNT_OWN_SERVICE));
        server1.MaxConcurrencyOf("test.EchoService.Echo") = 1;
        ASSERT_EQ(1, server1.MaxConcurrencyOf("test.EchoService.Echo"));
        server1.MaxConcurrencyOf(&service1, "Echo") = 2;
        ASSERT_EQ(2, server1.MaxConcurrencyOf(&service1, "Echo"));

        ASSERT_EQ(0, server1.Start(port, NULL));
        brpc::Channel http_channel;
        brpc::ChannelOptions chan_options;
        chan_options.protocol = "http";
        ASSERT_EQ(0, http_channel.Init("0.0.0.0", port, &chan_options));

        brpc::Channel normal_channel;
        ASSERT_EQ(0, normal_channel.Init("0.0.0.0", port, NULL));
        test::EchoService_Stub stub(&normal_channel);

        brpc::Controller cntl1;
        cntl1.http_request().uri() = "/EchoService/Echo";
        cntl1.http_request().set_method(brpc::HTTP_METHOD_POST);
        cntl1.request_attachment().append("{\"message\":\"hello\",\"sleep_us\":100000}");
        http_channel.CallMethod(NULL, &cntl1, NULL, NULL, brpc::DoNothing());

        brpc::Controller cntl2;
        test::EchoRequest req;
        test::EchoResponse res;
        req.set_message("hello");
        req.set_sleep_us(100000);
        stub.Echo(&cntl2, &req, &res, brpc::DoNothing());

        bthread_usleep(20000);
        LOG(INFO) << "Send other requests";

        brpc::Controller cntl3;
        cntl3.http_request().uri() = "/EchoService/Echo";
        cntl3.http_request().set_method(brpc::HTTP_METHOD_POST);
        cntl3.request_attachment().append("{\"message\":\"hello\"}");
        http_channel.CallMethod(NULL, &cntl3, NULL, NULL, NULL);
        ASSERT_TRUE(cntl3.Failed());
        ASSERT_EQ(brpc::ELIMIT, cntl3.ErrorCode());
        ASSERT_EQ(brpc::HTTP_STATUS_SERVICE_UNAVAILABLE, cntl3.http_response().status_code());

        brpc::Join(cntl1.call_id());
        brpc::Join(cntl2.call_id());
        ASSERT_FALSE(cntl1.Failed()) << cntl1.ErrorText();
        ASSERT_FALSE(cntl2.Failed()) << cntl2.ErrorText();
    }

    brpc::policy::FLAGS_use_http_error_code = false;
}

TEST_F(ServerTest, conflict_name_between_restful_mapping_and_builtin) {
    const int port = 9200;
    EchoServiceV1 service_v1;
    
    brpc::Server server1;
    ASSERT_EQ(0u, server1.service_count());
    ASSERT_EQ(0, server1.AddService(
                  &service_v1,
                  brpc::SERVER_DOESNT_OWN_SERVICE,
                  "/status/hello => Echo"));
    ASSERT_EQ(1u, server1.service_count());
    ASSERT_TRUE(server1._global_restful_map == NULL);

    ASSERT_EQ(-1, server1.Start(port, NULL));
}

TEST_F(ServerTest, restful_mapping_is_tried_after_others) {
    const int port = 9200;
    EchoServiceV1 service_v1;
    
    brpc::Server server1;
    ASSERT_EQ(0u, server1.service_count());
    ASSERT_EQ(0, server1.AddService(
                  &service_v1,
                  brpc::SERVER_DOESNT_OWN_SERVICE,
                  "* => Echo"));
    ASSERT_EQ(1u, server1.service_count());
    ASSERT_TRUE(server1._global_restful_map);
    ASSERT_EQ(1UL, server1._global_restful_map->size());

    ASSERT_EQ(0, server1.Start(port, NULL));
    
    brpc::Channel http_channel;
    brpc::ChannelOptions chan_options;
    chan_options.protocol = "http";
    ASSERT_EQ(0, http_channel.Init("0.0.0.0", port, &chan_options));

    // accessing /status should be OK.
    brpc::Controller cntl;
    cntl.http_request().uri() = "/status";
    http_channel.CallMethod(NULL, &cntl, NULL, NULL, NULL);
    ASSERT_FALSE(cntl.Failed()) << cntl.ErrorText();
    ASSERT_TRUE(cntl.response_attachment().to_string().find(
                  service_v1.GetDescriptor()->full_name()) != std::string::npos)
        << "body=" << cntl.response_attachment();

    // reject /EchoService/Echo
    cntl.Reset();
    cntl.http_request().uri() = "/EchoService/Echo";
    cntl.http_request().set_method(brpc::HTTP_METHOD_POST);
    cntl.request_attachment().append("{\"message\":\"foo\"}");
    http_channel.CallMethod(NULL, &cntl, NULL, NULL, NULL);
    ASSERT_TRUE(cntl.Failed());
    ASSERT_EQ(0, service_v1.ncalled.load());

    // Hit restful map
    cntl.Reset();
    cntl.http_request().uri() = "/non_exist";
    cntl.http_request().set_method(brpc::HTTP_METHOD_POST);
    cntl.request_attachment().append("{\"message\":\"foo\"}");
    http_channel.CallMethod(NULL, &cntl, NULL, NULL, NULL);
    ASSERT_FALSE(cntl.Failed()) << cntl.ErrorText();
    ASSERT_EQ(1, service_v1.ncalled.load());
    ASSERT_EQ("{\"message\":\"foo_v1\"}", cntl.response_attachment());
;
    //Stop the server.
    server1.Stop(0);
    server1.Join();

    // Removing the service should update _global_restful_map.
    ASSERT_EQ(0, server1.RemoveService(&service_v1));
    ASSERT_EQ(0u, server1.service_count());
    ASSERT_TRUE(server1._global_restful_map); // deleted in dtor.
    ASSERT_EQ(0u, server1._global_restful_map->size());

}

TEST_F(ServerTest, add_remove_service) {
    brpc::Server server;
    EchoServiceImpl echo_svc;
    ASSERT_EQ(0, server.AddService(
        &echo_svc, brpc::SERVER_DOESNT_OWN_SERVICE));
    // Duplicate
    ASSERT_EQ(-1, server.AddService(
        &echo_svc, brpc::SERVER_DOESNT_OWN_SERVICE));
    ASSERT_TRUE(server.FindServiceByName(
        test::EchoService::descriptor()->name()) == &echo_svc);
    ASSERT_TRUE(server.FindServiceByFullName(
        test::EchoService::descriptor()->full_name()) == &echo_svc);
    ASSERT_TRUE(NULL == server.FindServiceByFullName(
        test::EchoService::descriptor()->name()));

    butil::EndPoint ep;
    ASSERT_EQ(0, str2endpoint("127.0.0.1:8613", &ep));
    ASSERT_EQ(0, server.Start(ep, NULL));

    ASSERT_EQ(1ul, server.service_count());
    ASSERT_TRUE(server.first_service() == &echo_svc);
    ASSERT_TRUE(server.FindServiceByName(
        test::EchoService::descriptor()->name()) == &echo_svc);
    // Can't add/remove service while running
    ASSERT_EQ(-1, server.AddService(
        &echo_svc, brpc::SERVER_DOESNT_OWN_SERVICE));
    ASSERT_EQ(-1, server.RemoveService(&echo_svc));
    
    ASSERT_EQ(0, server.Stop(0));
    ASSERT_EQ(0, server.Join());

    ASSERT_EQ(0, server.RemoveService(&echo_svc));
    ASSERT_EQ(0ul, server.service_count());
    EchoServiceImpl* svc_on_heap = new EchoServiceImpl();
    ASSERT_EQ(0, server.AddService(svc_on_heap,
                                   brpc::SERVER_OWNS_SERVICE));
    ASSERT_EQ(0, server.RemoveService(svc_on_heap));
    ASSERT_TRUE(g_delete);

    server.ClearServices();
    ASSERT_EQ(0ul, server.service_count());
}

void SendSleepRPC(butil::EndPoint ep, int sleep_ms, bool succ) {
    brpc::Channel channel;
    ASSERT_EQ(0, channel.Init(ep, NULL));

    brpc::Controller cntl;
    test::EchoRequest req;
    test::EchoResponse res;
    req.set_message(EXP_REQUEST);
    if (sleep_ms > 0) {
        req.set_sleep_us(sleep_ms * 1000);
    }
    test::EchoService_Stub stub(&channel);
    stub.Echo(&cntl, &req, &res, NULL);
    if (succ) {
        EXPECT_FALSE(cntl.Failed()) << cntl.ErrorText()
                                    << " latency=" << cntl.latency_us();
    } else {
        EXPECT_TRUE(cntl.Failed());
    }
}

TEST_F(ServerTest, close_idle_connections) {
    butil::EndPoint ep;
    brpc::Server server;
    brpc::ServerOptions opt;
    opt.idle_timeout_sec = 1;
    ASSERT_EQ(0, str2endpoint("127.0.0.1:9776", &ep));
    ASSERT_EQ(0, server.Start(ep, &opt));

    const int cfd = tcp_connect(ep, NULL);
    ASSERT_GT(cfd, 0);
    usleep(10000);
    brpc::ServerStatistics stat;
    server.GetStat(&stat);
    ASSERT_EQ(1ul, stat.connection_count);

    usleep(2500000);
    server.GetStat(&stat);
    ASSERT_EQ(0ul, stat.connection_count);
}

TEST_F(ServerTest, logoff_and_multiple_start) {
    butil::Timer timer;
    butil::EndPoint ep;
    EchoServiceImpl echo_svc;
    brpc::Server server;
    ASSERT_EQ(0, server.AddService(&echo_svc,
                                   brpc::SERVER_DOESNT_OWN_SERVICE));
    ASSERT_EQ(0, str2endpoint("127.0.0.1:9876", &ep));
    
    // Server::Stop(-1)
    {
        ASSERT_EQ(0, server.Start(ep, NULL));
        bthread_t tid;
        const int64_t old_count = echo_svc.count.load(butil::memory_order_relaxed);
        google::protobuf::Closure* thrd_func = 
            brpc::NewCallback(SendSleepRPC, ep, 100, true);
        EXPECT_EQ(0, bthread_start_background(&tid, NULL, RunClosure, thrd_func));
        while (echo_svc.count.load(butil::memory_order_relaxed) == old_count) {
            bthread_usleep(1000);
        }
        timer.start();
        ASSERT_EQ(0, server.Stop(-1));
        ASSERT_EQ(0, server.Join());
        timer.stop();
        EXPECT_TRUE(labs(timer.m_elapsed() - 100) < 15) << timer.m_elapsed();
        bthread_join(tid, NULL);
    }

    // Server::Stop(0)
    {
        ++ep.port;
        ASSERT_EQ(0, server.Start(ep, NULL));
        bthread_t tid;
        const int64_t old_count = echo_svc.count.load(butil::memory_order_relaxed);
        google::protobuf::Closure* thrd_func = 
            brpc::NewCallback(SendSleepRPC, ep, 100, true);
        EXPECT_EQ(0, bthread_start_background(&tid, NULL, RunClosure, thrd_func));
        while (echo_svc.count.load(butil::memory_order_relaxed) == old_count) {
            bthread_usleep(1000);
        }
        
        timer.start();
        ASSERT_EQ(0, server.Stop(0));
        ASSERT_EQ(0, server.Join());
        timer.stop();
        // Assertion will fail since EchoServiceImpl::Echo is holding
        // additional reference to the `Socket'
        // EXPECT_TRUE(timer.m_elapsed() < 15) << timer.m_elapsed();
        bthread_join(tid, NULL);
    }

    // Server::Stop(timeout) where timeout < g_sleep_ms
    {
        ++ep.port;
        ASSERT_EQ(0, server.Start(ep, NULL));
        bthread_t tid;
        const int64_t old_count = echo_svc.count.load(butil::memory_order_relaxed);
        google::protobuf::Closure* thrd_func = 
            brpc::NewCallback(SendSleepRPC, ep, 100, true);
        EXPECT_EQ(0, bthread_start_background(&tid, NULL, RunClosure, thrd_func));
        while (echo_svc.count.load(butil::memory_order_relaxed) == old_count) {
            bthread_usleep(1000);
        }

        timer.start();
        ASSERT_EQ(0, server.Stop(50));
        ASSERT_EQ(0, server.Join());
        timer.stop();
        // Assertion will fail since EchoServiceImpl::Echo is holding
        // additional reference to the `Socket'
        // EXPECT_TRUE(labs(timer.m_elapsed() - 50) < 15) << timer.m_elapsed();
        bthread_join(tid, NULL);
    }
    
    // Server::Stop(timeout) where timeout > g_sleep_ms
    {
        ++ep.port;
        ASSERT_EQ(0, server.Start(ep, NULL));
        bthread_t tid;
        const int64_t old_count = echo_svc.count.load(butil::memory_order_relaxed);
        google::protobuf::Closure* thrd_func = 
            brpc::NewCallback(SendSleepRPC, ep, 100, true);
        EXPECT_EQ(0, bthread_start_background(&tid, NULL, RunClosure, thrd_func));
        while (echo_svc.count.load(butil::memory_order_relaxed) == old_count) {
            bthread_usleep(1000);
        }
        timer.start();
        ASSERT_EQ(0, server.Stop(1000));
        ASSERT_EQ(0, server.Join());
        timer.stop();
        EXPECT_TRUE(labs(timer.m_elapsed() - 100) < 15) << timer.m_elapsed();
        bthread_join(tid, NULL);
    }
}

void SendMultipleRPC(butil::EndPoint ep, int count) {
    brpc::Channel channel;
    EXPECT_EQ(0, channel.Init(ep, NULL));

    for (int i = 0; i < count; ++i) {
        brpc::Controller cntl;
        test::EchoRequest req;
        test::EchoResponse res;
        req.set_message(EXP_REQUEST);
        test::EchoService_Stub stub(&channel);
        stub.Echo(&cntl, &req, &res, NULL);
 
        EXPECT_EQ(EXP_RESPONSE, res.message()) << cntl.ErrorText();
    }
}
              
TEST_F(ServerTest, serving_requests) {
    EchoServiceImpl echo_svc;
    brpc::Server server;
    ASSERT_EQ(0, server.AddService(&echo_svc,
                                   brpc::SERVER_DOESNT_OWN_SERVICE));
    butil::EndPoint ep;
    ASSERT_EQ(0, str2endpoint("127.0.0.1:8613", &ep));
    ASSERT_EQ(0, server.Start(ep, NULL));

    const int NUM = 1;
    const int COUNT = 1;
    pthread_t tids[NUM];
    for (int i = 0; i < NUM; ++i) {
        google::protobuf::Closure* thrd_func = 
                brpc::NewCallback(SendMultipleRPC, ep, COUNT);
        EXPECT_EQ(0, pthread_create(&tids[i], NULL, RunClosure, thrd_func));
    }
    for (int i = 0; i < NUM; ++i) {
        pthread_join(tids[i], NULL);
    }
    ASSERT_EQ(NUM * COUNT, echo_svc.count.load());
    ASSERT_EQ(0, server.Stop(0));
    ASSERT_EQ(0, server.Join());
}

TEST_F(ServerTest, create_pid_file) {
    {
        brpc::Server server;
        server._options.pid_file = "./pid_dir/sub_dir/./.server.pid";
        server.PutPidFileIfNeeded();
        pid_t pid = getpid();
        std::ifstream fin("./pid_dir/sub_dir/.server.pid");
        ASSERT_TRUE(fin.is_open());
        pid_t pid_from_file;
        fin >> pid_from_file;
        ASSERT_EQ(pid, pid_from_file);
    }
    std::ifstream fin("./pid_dir/sub_dir/.server.pid");
    ASSERT_FALSE(fin.is_open());
}

TEST_F(ServerTest, range_start) {
    const int START_PORT = 8713;
    const int END_PORT = 8719;
    butil::fd_guard listen_fds[END_PORT - START_PORT];
    butil::EndPoint point;
    for (int i = START_PORT; i < END_PORT; ++i) {
        point.port = i;
        listen_fds[i - START_PORT].reset(butil::tcp_listen(point));
    }

    brpc::Server server;
    EXPECT_EQ(-1, server.Start("0.0.0.0", brpc::PortRange(START_PORT, END_PORT - 1), NULL));
    // note: add an extra port after END_PORT to detect the bug that the 
    // probing does not stop at the first valid port(END_PORT).
    EXPECT_EQ(0, server.Start("0.0.0.0", brpc::PortRange(START_PORT, END_PORT + 1/*note*/), NULL));
    EXPECT_EQ(END_PORT, server.listen_address().port);
}

TEST_F(ServerTest, add_builtin_service) {
    TestAddBuiltinService(brpc::IndexService::descriptor());
    TestAddBuiltinService(brpc::VersionService::descriptor());
    TestAddBuiltinService(brpc::HealthService::descriptor());
    TestAddBuiltinService(brpc::StatusService::descriptor());
    TestAddBuiltinService(brpc::ConnectionsService::descriptor());
    TestAddBuiltinService(brpc::BadMethodService::descriptor());
    TestAddBuiltinService(brpc::ListService::descriptor());
    if (brpc::FLAGS_enable_threads_service) {
        TestAddBuiltinService(brpc::ThreadsService::descriptor());
    }
#if !BRPC_WITH_GLOG
    TestAddBuiltinService(brpc::VLogService::descriptor());
#endif
    TestAddBuiltinService(brpc::FlagsService::descriptor());
    TestAddBuiltinService(brpc::VarsService::descriptor());
    TestAddBuiltinService(brpc::RpczService::descriptor());
    TestAddBuiltinService(brpc::PProfService::descriptor());
    if (brpc::FLAGS_enable_dir_service) {
        TestAddBuiltinService(brpc::DirService::descriptor());
    }
}

TEST_F(ServerTest, base64_to_string) {
    // We test two cases as following. If these two tests can be passed, we
    // can prove that the pb_bytes_to_base64 flag is working in both client side
    // and server side.
    // 1. Client sets pb_bytes_to_base64 and server also sets pb_bytes_to_base64
    // 2. Client sets pb_bytes_to_base64, but server doesn't set pb_bytes_to_base64
    for (int i = 0; i < 2; ++i) {
        brpc::Server server;
        EchoServiceImpl echo_svc;
        brpc::ServiceOptions service_opt;
        service_opt.pb_bytes_to_base64 = (i == 0);
        ASSERT_EQ(0, server.AddService(&echo_svc,
                                       service_opt));
        ASSERT_EQ(0, server.Start(8613, NULL));

        brpc::Channel chan;
        brpc::ChannelOptions opt;
        opt.protocol = brpc::PROTOCOL_HTTP;
        ASSERT_EQ(0, chan.Init("localhost:8613", &opt));
        brpc::Controller cntl;
        cntl.http_request().uri() = "/EchoService/BytesEcho" +
                butil::string_printf("%d", i + 1);
        cntl.http_request().set_method(brpc::HTTP_METHOD_POST);
        cntl.http_request().set_content_type("application/json");
        cntl.set_pb_bytes_to_base64(true);
        test::BytesRequest req;
        test::BytesResponse res;
        req.set_databytes(EXP_REQUEST);
        chan.CallMethod(NULL, &cntl, &req, &res, NULL);
        EXPECT_FALSE(cntl.Failed());
        EXPECT_EQ(EXP_REQUEST, res.databytes());
        server.Stop(0);
        server.Join();
    }
}

TEST_F(ServerTest, single_repeated_to_array) {
    for (int i = 0; i < 2; ++i) {
        brpc::Server server;
        EchoServiceImpl echo_svc;
        brpc::ServiceOptions service_opt;
        service_opt.pb_single_repeated_to_array = (i == 0);

        ASSERT_EQ(0, server.AddService(&echo_svc, service_opt));
        ASSERT_EQ(0, server.Start(8613, NULL));

        for (int j = 0; j < 2; ++j) {
            brpc::Channel chan;
            brpc::ChannelOptions opt;
            opt.protocol = brpc::PROTOCOL_HTTP;
            ASSERT_EQ(0, chan.Init("localhost:8613", &opt));
            brpc::Controller cntl;
            cntl.http_request().uri() = "/EchoService/ComboEcho";
            cntl.http_request().set_method(brpc::HTTP_METHOD_POST);
            cntl.http_request().set_content_type("application/json");
            cntl.set_pb_single_repeated_to_array(j == 0);
            test::ComboRequest req;
            req.add_requests()->set_message("foo");
            req.add_requests()->set_message("bar");

            test::ComboResponse res;
            chan.CallMethod(NULL, &cntl, &req, &res, NULL);
            if (i == j) {
                EXPECT_FALSE(cntl.Failed());
                EXPECT_EQ(res.responses_size(), req.requests_size());
                for (int k = 0; k < req.requests_size(); ++k) {
                    EXPECT_EQ(req.requests(k).message(), res.responses(k).message());
                }
            } else {
                EXPECT_TRUE(cntl.Failed());
            }
        }

        server.Stop(0);
        server.Join();
    }
}

TEST_F(ServerTest, too_big_message) {
    EchoServiceImpl echo_svc;
    brpc::Server server;
    ASSERT_EQ(0, server.AddService(&echo_svc,
                                   brpc::SERVER_DOESNT_OWN_SERVICE));
    ASSERT_EQ(0, server.Start(8613, NULL));

#if !BRPC_WITH_GLOG
    logging::StringSink log_str;
    logging::LogSink* old_sink = logging::SetLogSink(&log_str);
#endif

    brpc::Channel chan;
    ASSERT_EQ(0, chan.Init("localhost:8613", NULL));
    brpc::Controller cntl;
    test::EchoRequest req;
    test::EchoResponse res;
    req.mutable_message()->resize(brpc::FLAGS_max_body_size + 1);
    test::EchoService_Stub stub(&chan);
    stub.Echo(&cntl, &req, &res, NULL);
    EXPECT_TRUE(cntl.Failed());

#if !BRPC_WITH_GLOG
    ASSERT_EQ(&log_str, logging::SetLogSink(old_sink));
    std::ostringstream expected_log;
    expected_log << " is bigger than " << brpc::FLAGS_max_body_size
                 << " bytes, the connection will be closed."
                    " Set max_body_size to allow bigger messages";
    ASSERT_NE(std::string::npos, log_str.find(expected_log.str()));
#endif

    server.Stop(0);
    server.Join();
}

TEST_F(ServerTest, max_concurrency) {
    const int port = 9200;
    brpc::Server server1;
    EchoServiceImpl service1;
    ASSERT_EQ(0, server1.AddService(&service1, brpc::SERVER_DOESNT_OWN_SERVICE));
    server1.MaxConcurrencyOf("test.EchoService.Echo") = 1;
    ASSERT_EQ(1, server1.MaxConcurrencyOf("test.EchoService.Echo"));
    server1.MaxConcurrencyOf(&service1, "Echo") = 2;
    ASSERT_EQ(2, server1.MaxConcurrencyOf(&service1, "Echo")); 

    ASSERT_EQ(0, server1.Start(port, NULL));
    brpc::Channel http_channel;
    brpc::ChannelOptions chan_options;
    chan_options.protocol = "http";
    ASSERT_EQ(0, http_channel.Init("0.0.0.0", port, &chan_options));
    
    brpc::Channel normal_channel;
    ASSERT_EQ(0, normal_channel.Init("0.0.0.0", port, NULL));
    test::EchoService_Stub stub(&normal_channel);

    brpc::Controller cntl1;
    cntl1.http_request().uri() = "/EchoService/Echo";
    cntl1.http_request().set_method(brpc::HTTP_METHOD_POST);
    cntl1.request_attachment().append("{\"message\":\"hello\",\"sleep_us\":100000}");
    http_channel.CallMethod(NULL, &cntl1, NULL, NULL, brpc::DoNothing());

    brpc::Controller cntl2;
    test::EchoRequest req;
    test::EchoResponse res;
    req.set_message("hello");
    req.set_sleep_us(100000);
    stub.Echo(&cntl2, &req, &res, brpc::DoNothing());

    bthread_usleep(20000);
    LOG(INFO) << "Send other requests";
    
    brpc::Controller cntl3;
    cntl3.http_request().uri() = "/EchoService/Echo";
    cntl3.http_request().set_method(brpc::HTTP_METHOD_POST);
    cntl3.request_attachment().append("{\"message\":\"hello\"}");
    http_channel.CallMethod(NULL, &cntl3, NULL, NULL, NULL);
    ASSERT_TRUE(cntl3.Failed());
    ASSERT_EQ(brpc::EHTTP, cntl3.ErrorCode());
    ASSERT_EQ(brpc::HTTP_STATUS_SERVICE_UNAVAILABLE, cntl3.http_response().status_code());

    brpc::Controller cntl4;
    req.clear_sleep_us();
    stub.Echo(&cntl4, &req, NULL, NULL);
    ASSERT_TRUE(cntl4.Failed());
    ASSERT_EQ(brpc::ELIMIT, cntl4.ErrorCode());
    
    brpc::Join(cntl1.call_id());
    brpc::Join(cntl2.call_id());
    ASSERT_FALSE(cntl1.Failed()) << cntl1.ErrorText();
    ASSERT_FALSE(cntl2.Failed()) << cntl2.ErrorText();

    cntl3.Reset();
    cntl3.http_request().uri() = "/EchoService/Echo";
    cntl3.http_request().set_method(brpc::HTTP_METHOD_POST);
    cntl3.request_attachment().append("{\"message\":\"hello\"}");
    http_channel.CallMethod(NULL, &cntl3, NULL, NULL, NULL);
    ASSERT_FALSE(cntl3.Failed()) << cntl3.ErrorText();

    cntl4.Reset();
    stub.Echo(&cntl4, &req, NULL, NULL);
    ASSERT_FALSE(cntl4.Failed()) << cntl4.ErrorText();
}

TEST_F(ServerTest, user_fields) {
    const int port = 9200;
    brpc::Server server;
    EchoServiceImpl service;
    ASSERT_EQ(0, server.AddService(&service, brpc::SERVER_DOESNT_OWN_SERVICE));
    ASSERT_EQ(0, server.Start(port, NULL));

    brpc::Channel channel;
    ASSERT_EQ(0, channel.Init("0.0.0.0", port, NULL));
    test::EchoService_Stub stub(&channel);

    brpc::Controller cntl;
    cntl.request_user_fields()->insert(EXP_USER_FIELD_KEY, EXP_USER_FIELD_VALUE);
    test::EchoRequest req;
    test::EchoResponse res;
    req.set_message("hello");
    stub.Echo(&cntl, &req, &res, NULL);

    ASSERT_FALSE(cntl.Failed()) << cntl.ErrorText();
    ASSERT_TRUE(cntl.has_response_user_fields());
    ASSERT_TRUE(!cntl.response_user_fields()->empty());
    std::string* val = cntl.response_user_fields()->seek(EXP_USER_FIELD_KEY);
    ASSERT_TRUE(val != NULL);
    ASSERT_EQ(*val, EXP_USER_FIELD_VALUE);
}

class BaiduMasterServiceImpl : public brpc::BaiduMasterService {
public:
    void ProcessRpcRequest(brpc::Controller* cntl,
                           const brpc::SerializedRequest* request,
                           brpc::SerializedResponse* response,
                           ::google::protobuf::Closure* done) override {
        // This object helps you to call done->Run() in RAII style. If you need
        // to process the request asynchronously, pass done_guard.release().
        brpc::ClosureGuard done_guard(done);
        ASSERT_NE(nullptr, cntl->sampled_request());
        ASSERT_TRUE(cntl->sampled_request()->meta.has_service_name());
        ASSERT_EQ(test::EchoService::descriptor()->full_name(),
                  cntl->sampled_request()->meta.service_name());
        ASSERT_TRUE(cntl->sampled_request()->meta.has_method_name());
        ASSERT_EQ("Echo", cntl->sampled_request()->meta.method_name());
        brpc::ContentType content_type = cntl->request_content_type();
        brpc::CompressType compress_type = cntl->request_compress_type();
        brpc::ChecksumType checksum_type = cntl->request_checksum_type();

        test::EchoRequest echo_request;
        test::EchoResponse echo_response;
        ASSERT_TRUE(brpc::policy::DeserializeRpcMessage(
            request->serialized_data(), *cntl, content_type, compress_type,
            checksum_type, &echo_request));
        ASSERT_EQ(EXP_REQUEST, echo_request.message());
        ASSERT_EQ(EXP_REQUEST, cntl->request_attachment().to_string());

        content_type = (brpc::ContentType)_content_type_index;
        compress_type = (brpc::CompressType)_compress_type_index;
        ++_compress_type_index;
        if (_compress_type_index == brpc::COMPRESS_TYPE_LZ4) {
            ++_compress_type_index;
        }
        if (_compress_type_index > brpc::CompressType_MAX) {
            _compress_type_index = brpc::CompressType_MIN;

            ++_content_type_index;
            if (_content_type_index > brpc::ContentType_MAX) {
                _content_type_index = brpc::ContentType_MIN;
            }
        }

        cntl->set_response_content_type(content_type);
        cntl->set_response_compress_type(compress_type);
        cntl->set_response_checksum_type(checksum_type);
        cntl->response_attachment().append(EXP_RESPONSE);
        echo_response.set_message(EXP_RESPONSE);
        ASSERT_TRUE(brpc::policy::SerializeRpcMessage(
            echo_response, *cntl, content_type, compress_type, checksum_type,
            &response->serialized_data()));
    }
private:
    int _content_type_index = brpc::ContentType_MIN;
    int _compress_type_index = brpc::CompressType_MIN;
};

void TestBaiduMasterService(brpc::Channel& channel, brpc::CompressType compress_type) {
    brpc::Controller cntl;
    test::EchoRequest req;
    test::EchoResponse res;
    req.set_message(EXP_REQUEST);
    cntl.request_attachment().append(EXP_REQUEST);
    cntl.set_request_compress_type(compress_type);
    test::EchoService_Stub stub(&channel);
    stub.Echo(&cntl, &req, &res, NULL);
    ASSERT_FALSE(cntl.Failed()) << cntl.ErrorText();
    ASSERT_EQ(EXP_RESPONSE, res.message());
    ASSERT_EQ(EXP_RESPONSE, cntl.response_attachment().to_string());
}

TEST_F(ServerTest, baidu_master_service) {
    butil::EndPoint ep;
    ASSERT_EQ(0, str2endpoint("127.0.0.1:8613", &ep));
    brpc::Server server;
    EchoServiceImpl service;
    ASSERT_EQ(0, server.AddService(&service, brpc::SERVER_DOESNT_OWN_SERVICE));
    brpc::ServerOptions server_options;
    server_options.baidu_master_service = new BaiduMasterServiceImpl;
    ASSERT_EQ(0, server.Start(ep, &server_options));

    brpc::Channel channel;
    brpc::ChannelOptions channel_options;
    channel_options.protocol = "baidu_std";
    ASSERT_EQ(0, channel.Init(ep, &channel_options));

    for (int i = 0; i < 10; ++i) {
        TestBaiduMasterService(channel, brpc::COMPRESS_TYPE_ZLIB);
        TestBaiduMasterService(channel, brpc::COMPRESS_TYPE_GZIP);
        TestBaiduMasterService(channel, brpc::COMPRESS_TYPE_SNAPPY);
        TestBaiduMasterService(channel, brpc::COMPRESS_TYPE_NONE);
    }

    ASSERT_EQ(0, server.Stop(0));
    ASSERT_EQ(0, server.Join());
}

void TestGenericCall(brpc::Channel& channel, brpc::ContentType content_type,
                     brpc::CompressType compress_type,
                     brpc::ChecksumType checksum_type) {
    LOG(INFO) << "TestGenericCall: content_type=" << content_type
              << ", compress_type=" << compress_type
              << ", checksum_type=" << checksum_type;
    test::EchoRequest request;
    test::EchoResponse response;
    request.set_message(EXP_REQUEST);

    brpc::SerializedResponse serialized_response;
    brpc::SerializedRequest serialized_request;

    brpc::Controller cntl;
    cntl.set_request_content_type(content_type);
    cntl.set_request_compress_type(compress_type);
    cntl.set_request_checksum_type(checksum_type);
    cntl.request_attachment().append(EXP_REQUEST);

    std::string error;
    ASSERT_TRUE(brpc::policy::SerializeRpcMessage(
        request, cntl, content_type, compress_type, checksum_type,
        &serialized_request.serialized_data()));
    auto sampled_request = new (std::nothrow) brpc::SampledRequest();
    sampled_request->meta.set_service_name(
        test::EchoService::descriptor()->full_name());
    sampled_request->meta.set_method_name(
        test::EchoService::descriptor()->FindMethodByName("Echo")->name());
    cntl.reset_sampled_request(sampled_request);

    channel.CallMethod(NULL, &cntl, &serialized_request, &serialized_response, NULL);
    ASSERT_FALSE(cntl.Failed()) << cntl.ErrorText();

    ASSERT_TRUE(brpc::policy::DeserializeRpcMessage(
        serialized_response.serialized_data(), cntl,
        cntl.response_content_type(), cntl.response_compress_type(),
        cntl.response_checksum_type(), &response));
    ASSERT_EQ(EXP_RESPONSE, response.message());
    ASSERT_EQ(EXP_RESPONSE, cntl.response_attachment().to_string());
}

TEST_F(ServerTest, generic_call) {
    butil::EndPoint ep;
    ASSERT_EQ(0, str2endpoint("127.0.0.1:8613", &ep));
    brpc::Server server;
    EchoServiceImpl service;
    ASSERT_EQ(0, server.AddService(&service, brpc::SERVER_DOESNT_OWN_SERVICE));
    brpc::ServerOptions server_options;
    server_options.baidu_master_service = new BaiduMasterServiceImpl;
    ASSERT_EQ(0, server.Start(ep, &server_options));

    brpc::Channel channel;
    brpc::ChannelOptions channel_options;
    channel_options.protocol = "baidu_std";
    ASSERT_EQ(0, channel.Init(ep, &channel_options));

    TestGenericCall(channel, brpc::CONTENT_TYPE_PB, brpc::COMPRESS_TYPE_ZLIB,
                    brpc::CHECKSUM_TYPE_CRC32C);
    TestGenericCall(channel, brpc::CONTENT_TYPE_PB, brpc::COMPRESS_TYPE_GZIP,
                    brpc::CHECKSUM_TYPE_CRC32C);
    TestGenericCall(channel, brpc::CONTENT_TYPE_PB, brpc::COMPRESS_TYPE_SNAPPY,
                    brpc::CHECKSUM_TYPE_NONE);
    TestGenericCall(channel, brpc::CONTENT_TYPE_PB, brpc::COMPRESS_TYPE_NONE,
                    brpc::CHECKSUM_TYPE_NONE);

    TestGenericCall(channel, brpc::CONTENT_TYPE_JSON, brpc::COMPRESS_TYPE_ZLIB,
                    brpc::CHECKSUM_TYPE_CRC32C);
    TestGenericCall(channel, brpc::CONTENT_TYPE_JSON, brpc::COMPRESS_TYPE_GZIP,
                    brpc::CHECKSUM_TYPE_CRC32C);
    TestGenericCall(channel, brpc::CONTENT_TYPE_JSON,
                    brpc::COMPRESS_TYPE_SNAPPY, brpc::CHECKSUM_TYPE_NONE);
    TestGenericCall(channel, brpc::CONTENT_TYPE_JSON, brpc::COMPRESS_TYPE_NONE,
                    brpc::CHECKSUM_TYPE_NONE);

    TestGenericCall(channel, brpc::CONTENT_TYPE_PROTO_JSON,
                    brpc::COMPRESS_TYPE_ZLIB, brpc::CHECKSUM_TYPE_CRC32C);
    TestGenericCall(channel, brpc::CONTENT_TYPE_PROTO_JSON,
                    brpc::COMPRESS_TYPE_GZIP, brpc::CHECKSUM_TYPE_CRC32C);
    TestGenericCall(channel, brpc::CONTENT_TYPE_PROTO_JSON,
                    brpc::COMPRESS_TYPE_SNAPPY, brpc::CHECKSUM_TYPE_NONE);
    TestGenericCall(channel, brpc::CONTENT_TYPE_PROTO_JSON,
                    brpc::COMPRESS_TYPE_NONE, brpc::CHECKSUM_TYPE_NONE);

    TestGenericCall(channel, brpc::CONTENT_TYPE_PROTO_TEXT,
                    brpc::COMPRESS_TYPE_ZLIB, brpc::CHECKSUM_TYPE_CRC32C);
    TestGenericCall(channel, brpc::CONTENT_TYPE_PROTO_TEXT,
                    brpc::COMPRESS_TYPE_GZIP, brpc::CHECKSUM_TYPE_CRC32C);
    TestGenericCall(channel, brpc::CONTENT_TYPE_PROTO_TEXT,
                    brpc::COMPRESS_TYPE_SNAPPY, brpc::CHECKSUM_TYPE_NONE);
    TestGenericCall(channel, brpc::CONTENT_TYPE_PROTO_TEXT,
                    brpc::COMPRESS_TYPE_NONE, brpc::CHECKSUM_TYPE_NONE);

    ASSERT_EQ(0, server.Stop(0));
    ASSERT_EQ(0, server.Join());
}

struct DefaultRpcPBMessages : public brpc::RpcPBMessages {
    DefaultRpcPBMessages() : request(NULL), response(NULL) {}
    ::google::protobuf::Message* Request() override { return request; }
    ::google::protobuf::Message* Response() override { return response; }

    ::google::protobuf::Message* request;
    ::google::protobuf::Message* response;
};

class TestRpcPBMessageFactory : public brpc::RpcPBMessageFactory {
public:
    brpc::RpcPBMessages* Get(const google::protobuf::Service& service,
                             const google::protobuf::MethodDescriptor& method) override {
        auto messages = butil::get_object<DefaultRpcPBMessages>();
        auto request = butil::get_object<v1::EchoRequest>();
        auto response = butil::get_object<v1::EchoResponse>();
        request->clear_message();
        response->clear_message();
        messages->request = request;
        messages->response = response;
        return messages;
    }

    void Return(brpc::RpcPBMessages* messages) override {
        auto test_messages = static_cast<DefaultRpcPBMessages*>(messages);
        butil::return_object(static_cast<v1::EchoRequest*>(test_messages->request));
        butil::return_object(static_cast<v1::EchoResponse*>(test_messages->response));
        test_messages->request = NULL;
        test_messages->response = NULL;
        butil::return_object(test_messages);
    }
};

TEST_F(ServerTest, rpc_pb_message_factory) {
    butil::EndPoint ep;
    ASSERT_EQ(0, str2endpoint("127.0.0.1:8613", &ep));
    brpc::Server server;
    EchoServiceV1 service;
    ASSERT_EQ(0, server.AddService(&service, brpc::SERVER_DOESNT_OWN_SERVICE));
    brpc::ServerOptions opt;
    opt.rpc_pb_message_factory = new TestRpcPBMessageFactory;
    ASSERT_EQ(0, server.Start(ep, &opt));

    brpc::Channel baidu_chan;
    brpc::ChannelOptions baidu_copt;
    baidu_copt.protocol = "baidu_std";
    ASSERT_EQ(0, baidu_chan.Init(ep, &baidu_copt));
    for (int i = 0; i < 1000; ++i) {
        brpc::Controller cntl;
        v1::EchoRequest req;
        v1::EchoResponse res;
        req.set_message(EXP_REQUEST);
        v1::EchoService_Stub stub(&baidu_chan);
        stub.Echo(&cntl, &req, &res, NULL);
        ASSERT_FALSE(cntl.Failed()) << cntl.ErrorText();
        ASSERT_EQ(EXP_REQUEST + "_v1", res.message());
    }

    brpc::Channel http_chan;
    brpc::ChannelOptions http_copt;
    http_copt.protocol = "http";
    ASSERT_EQ(0, http_chan.Init(ep, &http_copt));
    for (int i = 0; i < 1000; ++i) {
        brpc::Controller cntl;
        cntl.request_attachment().append(
            butil::string_printf(R"({"message":"%s"})", EXP_REQUEST.c_str()));
        v1::EchoService_Stub stub(&http_chan);
        stub.Echo(&cntl, NULL, NULL, NULL);
        ASSERT_FALSE(cntl.Failed()) << cntl.ErrorText();
        ASSERT_EQ(butil::string_printf(R"({"message":"%s_v1"})", EXP_REQUEST.c_str()),
                  cntl.response_attachment().to_string());
    }

    ASSERT_EQ(0, server.Stop(0));
    ASSERT_EQ(0, server.Join());
}

TEST_F(ServerTest, arena_rpc_pb_message_factory) {
    butil::EndPoint ep;
    ASSERT_EQ(0, str2endpoint("127.0.0.1:8613", &ep));
    brpc::Server server;
    EchoServiceV3 service;
    ASSERT_EQ(0, server.AddService(&service, brpc::SERVER_DOESNT_OWN_SERVICE));
    brpc::ServerOptions opt;
    opt.rpc_pb_message_factory = brpc::GetArenaRpcPBMessageFactory();
    ASSERT_EQ(0, server.Start(ep, &opt));

    brpc::Channel baidu_chan;
    brpc::ChannelOptions baidu_copt;
    baidu_copt.protocol = "baidu_std";
    ASSERT_EQ(0, baidu_chan.Init(ep, &baidu_copt));
    for (int i = 0; i < 1000; ++i) {
        brpc::Controller cntl;
        v3::EchoRequest req;
        v3::EchoResponse res;
        req.set_message(EXP_REQUEST);
        v3::EchoService_Stub stub(&baidu_chan);
        stub.Echo(&cntl, &req, &res, NULL);
        ASSERT_FALSE(cntl.Failed()) << cntl.ErrorText();
        ASSERT_EQ(EXP_RESPONSE, res.message());
    }

    brpc::Channel http_chan;
    brpc::ChannelOptions http_copt;
    http_copt.protocol = "http";
    ASSERT_EQ(0, http_chan.Init(ep, &http_copt));
    for (int i = 0; i < 1000; ++i) {
        brpc::Controller cntl;
        cntl.request_attachment().append(
            butil::string_printf(R"({"message":"%s"})", EXP_REQUEST.c_str()));
        v3::EchoService_Stub stub(&http_chan);
        stub.Echo(&cntl, NULL, NULL, NULL);
        ASSERT_FALSE(cntl.Failed()) << cntl.ErrorText();
        ASSERT_EQ(butil::string_printf(R"({"message":"%s"})", EXP_RESPONSE.c_str()),
            cntl.response_attachment().to_string());
    }

    ASSERT_EQ(0, server.Stop(0));
    ASSERT_EQ(0, server.Join());
}

void TestBaiduStdAuth(const butil::EndPoint& ep,
    brpc::Controller& cntl,
    int error_code, bool failed) {
    brpc::Channel chan;
    brpc::ChannelOptions copt;
    copt.max_retry = 0;
    copt.protocol = "baidu_std";
    ASSERT_EQ(0, chan.Init(ep, &copt));

    test::EchoRequest req;
    test::EchoResponse res;
    req.set_message(EXP_REQUEST);
    test::EchoService_Stub stub(&chan);
    stub.Echo(&cntl, &req, &res, NULL);
    ASSERT_EQ(cntl.Failed(), failed) << cntl.ErrorText();
    ASSERT_EQ(cntl.ErrorCode(), error_code);
}

void TestHttpAuth(const butil::EndPoint& ep,
                  brpc::Controller& cntl,
                  int status_code, bool failed) {
    brpc::Channel chan;
    brpc::ChannelOptions copt;
    copt.max_retry = 0;
    copt.protocol = "http";
    ASSERT_EQ(0, chan.Init(ep, &copt));

    cntl.http_request().uri() = "/EchoService/Echo";
    cntl.request_attachment().append(R"({"message": "hello"})");
    cntl.http_request().set_method(brpc::HTTP_METHOD_POST);
    test::EchoService_Stub stub(&chan);
    chan.CallMethod(NULL, &cntl, NULL, NULL, NULL);
    ASSERT_EQ(cntl.Failed(), failed) << cntl.ErrorText();
    ASSERT_EQ(cntl.http_response().status_code(), status_code);
}

TEST_F(ServerTest, auth) {
    butil::EndPoint ep;
    ASSERT_EQ(0, str2endpoint("127.0.0.1:8613", &ep));
    brpc::Server server;
    EchoServiceImpl service;
    ASSERT_EQ(0, server.AddService(&service, brpc::SERVER_DOESNT_OWN_SERVICE));
    MyAuthenticator auth;
    brpc::ServerOptions opt;
    opt.auth = &auth;
    ASSERT_EQ(0, server.Start(ep, &opt));

    brpc::Controller cntl;
    TestBaiduStdAuth(ep, cntl, 0, false);

    g_verify_success = false;
    cntl.Reset();
    TestBaiduStdAuth(ep, cntl, brpc::ERPCAUTH, true);
    ASSERT_NE(cntl.ErrorText().find(g_unauthorized_error_text), std::string::npos);

    cntl.Reset();
    TestHttpAuth(ep, cntl, brpc::HTTP_STATUS_FORBIDDEN, true);
    ASSERT_NE(cntl.response_attachment().to_string().find(g_unauthorized_error_text),
              std::string::npos);

    g_verify_success = true;
    cntl.Reset();
    cntl.http_request().SetHeader("Authorization", "123");
    TestHttpAuth(ep, cntl, brpc::HTTP_STATUS_OK, false);

    ASSERT_EQ(0, server.Stop(0));
    ASSERT_EQ(0, server.Join());
}

} //namespace
