
parent_dir=$(patsubst %/,%,$(dir $(patsubst %.,%../foo,$(patsubst %..,%../../foo,$(1)))))

THIS_MAKE := $(lastword $(MAKEFILE_LIST))
ANON_ROOT := $(call parent_dir,$(call parent_dir,$(call parent_dir,$(THIS_MAKE))))

SOURCES+=\
$(ANON_ROOT)/examples/echo/main.cpp\
$(ANON_ROOT)/src/cpp/io_dispatch.cpp\
$(ANON_ROOT)/src/cpp/fiber.cpp\
$(ANON_ROOT)/src/cpp/tcp_server.cpp\
$(ANON_ROOT)/src/cpp/lock_checker.cpp\
$(ANON_ROOT)/src/cpp/http_server.cpp\
$(ANON_ROOT)/src/cpp/tls_context.cpp\
$(ANON_ROOT)/src/cpp/tls_pipe.cpp\
$(ANON_ROOT)/../http-parser/http_parser.c

INC_DIRS+=\
$(ANON_ROOT)/src/cpp\
$(ANON_ROOT)/../http-parser

