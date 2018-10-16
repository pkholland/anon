
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
$(ANON_PARENT)/http-parser/http_parser.c

ifneq ($(TEFLON_AWS),)
SOURCES+=$(ANON_ROOT)/src/cpp/aws_http.cpp
cflags+=-DTEFLON_AWS
LIBS:=-laws-cpp-sdk-s3 -laws-cpp-sdk-core -lcurl $(LIBS)
endif

ifneq ($(filter SQS,$(TEFLON_AWS)),)
SOURCES+=$(ANON_ROOT)/src/cpp/aws_sqs.cpp
INC_DIRS+=$(ANON_PARENT)/json/include
cflags+=-DTEFLON_AWS_SQS
LIBS:=-laws-cpp-sdk-sqs $(LIBS)
endif

ifneq ($(filter S3,$(TEFLON_AWS)),)
LIBS:=-laws-cpp-sdk-s3 $(LIBS)
cflags+=-DTEFLON_AWS_S3
endif

ifneq ($(filter DDB,$(TEFLON_AWS)),)
LIBS:=-laws-cpp-sdk-dynamodb $(LIBS)
cflags+=-DTEFLON_AWS_DDB
endif

ifneq ($(TEFLON_REQUEST_DISPATCHER),)
LIBS:=-lpcrecpp $(LIBS)
SOURCES+=$(ANON_ROOT)/src/cpp/request_dispatcher.cpp
cflags+=-DTEFLON_REQUEST_DISPATCHER
endif

INC_DIRS+=\
$(ANON_ROOT)/src/cpp\
$(ANON_PARENT)/http-parser\


