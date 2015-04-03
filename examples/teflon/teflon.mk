
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
$(ANON_PARENT)/http-parser/http_parser.c

INC_DIRS+=\
$(ANON_ROOT)/src/cpp\
$(ANON_PARENT)/http-parser\


