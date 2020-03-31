
parent_dir=$(patsubst %/,%,$(dir $(patsubst %.,%../foo,$(patsubst %..,%../../foo,$1))))

THIS_MAKE := $(lastword $(MAKEFILE_LIST))
ANON_ROOT := $(call parent_dir,$(call parent_dir,$(call parent_dir,$(THIS_MAKE))))
ANON_PARENT := $(patsubst ./..%,..%,$(ANON_ROOT)/..)
ANON_PARENT := $(patsubst ../anon/..,..,$(ANON_PARENT))

SOURCES+=\
$(ANON_ROOT)/examples/teflon/main.cpp\
$(ANON_ROOT)/src/cpp/io_dispatch.cpp\
$(ANON_ROOT)/src/cpp/fiber.cpp\
$(ANON_ROOT)/src/cpp/tcp_server.cpp\
$(ANON_ROOT)/src/cpp/tcp_client.cpp\
$(ANON_ROOT)/src/cpp/lock_checker.cpp\
$(ANON_ROOT)/src/cpp/http_server.cpp\
$(ANON_ROOT)/src/cpp/http_client.cpp\
$(ANON_ROOT)/src/cpp/tls_context.cpp\
$(ANON_ROOT)/src/cpp/tls_pipe.cpp\
$(ANON_ROOT)/src/cpp/dns_lookup.cpp\
$(ANON_ROOT)/src/cpp/dns_cache.cpp\
$(ANON_ROOT)/src/cpp/epc.cpp\
$(ANON_ROOT)/src/cpp/mcdc.cpp\
$(ANON_ROOT)/src/cpp/b64.cpp\
$(ANON_ROOT)/src/cpp/percent_codec.cpp\
$(ANON_ROOT)/src/cpp/big_id_crypto.cpp\
$(ANON_ROOT)/src/cpp/exe_cmd.cpp\
$(ANON_PARENT)/http-parser/http_parser.c

ifneq ($(ANON_AWS),)
SOURCES+=\
$(ANON_ROOT)/src/cpp/aws_http.cpp\
$(ANON_ROOT)/src/cpp/aws_client.cpp\
$(ANON_ROOT)/src/cpp/aws_throttle.cpp
INC_DIRS+=$(ANON_PARENT)/json/include
cflags+=-DANON_AWS
LIBS:=-laws-cpp-sdk-s3 -laws-cpp-sdk-core -lcurl $(LIBS)
endif

ifneq ($(filter SQS,$(ANON_AWS)),)
SOURCES+=$(ANON_ROOT)/src/cpp/aws_sqs.cpp
cflags+=-DANON_AWS_SQS
LIBS:=-laws-cpp-sdk-sqs $(LIBS)
endif

ifneq ($(filter S3,$(ANON_AWS)),)
LIBS:=-laws-cpp-sdk-s3 $(LIBS)
cflags+=-DANON_AWS_S3
endif

ifneq ($(filter DDB,$(ANON_AWS)),)
LIBS:=-laws-cpp-sdk-dynamodb $(LIBS)
cflags+=-DANON_AWS_DDB
endif

ifneq ($(filter ROUTE53,$(ANON_AWS)),)
LIBS:=-laws-cpp-sdk-route53 $(LIBS)
cflags+=-DANON_AWS_ROUTE53
endif

ifneq ($(filter EC2,$(ANON_AWS)),)
LIBS:=-laws-cpp-sdk-ec2 $(LIBS)
cflags+=-DANON_AWS_EC2
endif

ifneq ($(filter ACM,$(ANON_AWS)),)
LIBS:=-laws-cpp-sdk-acm $(LIBS)
cflags+=-DANON_AWS_ACM
endif

ifneq ($(filter ELBV2,$(ANON_AWS)),)
LIBS:=-laws-cpp-sdk-elasticloadbalancingv2 $(LIBS)
cflags+=-DANON_AWS_ELBV2
endif

ifneq ($(filter ACCEL,$(ANON_AWS)),)
LIBS:=-laws-cpp-sdk-globalaccelerator $(LIBS)
cflags+=-DANON_AWS_ACCEL
endif

ifneq ($(filter AUTOSCALING,$(ANON_AWS)),)
LIBS:=-laws-cpp-sdk-autoscaling $(LIBS)
cflags+=-DANON_AWS_AUTOSCALING
endif

ifneq ($(TEFLON_REQUEST_DISPATCHER),)
LIBS:=-lpcrecpp $(LIBS)
SOURCES+=\
$(ANON_ROOT)/src/cpp/request_dispatcher.cpp\
$(ANON_ROOT)/src/cpp/http_error.cpp
cflags+=-DTEFLON_REQUEST_DISPATCHER
endif

INC_DIRS+=\
$(ANON_ROOT)/src/cpp\
$(ANON_PARENT)/http-parser\


