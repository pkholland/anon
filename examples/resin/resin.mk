
parent_dir=$(patsubst %/,%,$(dir $(patsubst %.,%../foo,$(patsubst %..,%../../foo,$1))))

THIS_MAKE := $(lastword $(MAKEFILE_LIST))
ANON_ROOT := $(call parent_dir,$(call parent_dir,$(call parent_dir,$(THIS_MAKE))))
ANON_PARENT := $(patsubst ./..%,..%,$(ANON_ROOT)/..)
ANON_PARENT := $(patsubst ../anon/..,..,$(ANON_PARENT))

SOURCES+=\
$(ANON_ROOT)/examples/resin/main.cpp\
$(ANON_ROOT)/examples/resin/worker.cpp\
$(ANON_ROOT)/examples/resin/server.cpp\
$(ANON_ROOT)/examples/resin/server_control.cpp\
$(ANON_ROOT)/examples/resin/sync_teflon_app.cpp\
$(ANON_ROOT)/src/cpp/sproc_mgr.cpp\
$(ANON_ROOT)/src/cpp/io_dispatch.cpp\
$(ANON_ROOT)/src/cpp/fiber.cpp\
$(ANON_ROOT)/src/cpp/lock_checker.cpp\
$(ANON_ROOT)/src/cpp/exe_cmd.cpp\
$(ANON_PARENT)/http-parser/http_parser.c\


INC_DIRS+=$(ANON_PARENT)/json/include
LIBS:=-laws-cpp-sdk-ec2 -laws-cpp-sdk-dynamodb -laws-cpp-sdk-sqs -laws-cpp-sdk-s3 -laws-cpp-sdk-sns -laws-cpp-sdk-core -lcurl $(LIBS)
