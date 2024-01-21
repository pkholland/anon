parent_dir=$(patsubst %/,%,$(dir $(patsubst %.,%../foo,$(patsubst %..,%../../foo,$1))))

THIS_MAKE := $(lastword $(MAKEFILE_LIST))
ANON_ROOT := $(call parent_dir,$(call parent_dir,$(call parent_dir,$(THIS_MAKE))))
ANON_PARENT := $(patsubst ./..%,..%,$(ANON_ROOT)/..)
ANON_PARENT := $(patsubst ../anon/..,..,$(ANON_PARENT))

SOURCES+=\
$(ANON_ROOT)/examples/ffmpeg_runner/main.cpp\
$(ANON_ROOT)/examples/resin/worker_message.proto\
$(ANON_ROOT)/src/cpp/fiber.cpp\
$(ANON_ROOT)/src/cpp/io_dispatch.cpp\
$(ANON_ROOT)/src/cpp/aws_client.cpp\
$(ANON_ROOT)/src/cpp/aws_http.cpp\
$(ANON_ROOT)/src/cpp/tcp_client.cpp\
$(ANON_ROOT)/src/cpp/dns_cache.cpp\
$(ANON_ROOT)/src/cpp/dns_lookup.cpp\
$(ANON_ROOT)/src/cpp/http_client.cpp\
$(ANON_ROOT)/src/cpp/tls_context.cpp\
$(ANON_ROOT)/src/cpp/tls_pipe.cpp\
$(ANON_ROOT)/src/cpp/epc.cpp\
$(ANON_ROOT)/src/cpp/big_id_crypto.cpp\
$(ANON_PARENT)/http-parser/http_parser.c


INC_DIRS+=$(ANON_ROOT)/src/cpp $(ANON_ROOT)/examples/resin
LIBS:=-lprotobuf-lite $(LIBS)
