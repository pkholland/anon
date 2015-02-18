
parent_dir=$(patsubst %/,%,$(dir $(patsubst %.,%../foo,$(patsubst %..,%../../foo,$(1)))))

THIS_MAKE := $(lastword $(MAKEFILE_LIST))
ANON_ROOT := $(call parent_dir,$(call parent_dir,$(call parent_dir,$(THIS_MAKE))))
ANON_PARENT := $(patsubst ./..%,..%,$(ANON_ROOT)/..)
ANON_PARENT := $(patsubst ../anon/..,..,$(ANON_PARENT))

SOURCES+=\
$(ANON_ROOT)/examples/test/main.cpp\
$(ANON_ROOT)/src/cpp/io_dispatch.cpp\
$(ANON_ROOT)/src/cpp/udp_dispatch.cpp\
$(ANON_ROOT)/src/cpp/big_id_crypto.cpp\
$(ANON_ROOT)/src/cpp/fiber.cpp\
$(ANON_ROOT)/src/cpp/tcp_server.cpp\
$(ANON_ROOT)/src/cpp/tcp_client.cpp\
$(ANON_ROOT)/src/cpp/dns_cache.cpp\
$(ANON_ROOT)/src/cpp/dns_lookup.cpp\
$(ANON_ROOT)/src/cpp/lock_checker.cpp\
$(ANON_ROOT)/src/cpp/http_server.cpp\
$(ANON_ROOT)/src/cpp/tls_context.cpp\
$(ANON_ROOT)/src/cpp/tls_pipe.cpp\
$(ANON_ROOT)/src/cpp/epc.cpp\
$(ANON_PARENT)/http-parser/http_parser.c\
#$(ANON_ROOT)/examples/test/http2_test.cpp\
#$(ANON_ROOT)/src/cpp/b64.cpp\
#$(ANON_ROOT)/src/cpp/http2.cpp\
#$(ANON_ROOT)/src/cpp/http2_handler.cpp\
#$(ANON_PARENT)/proxygen/proxygen/lib/http/codec/compress/HPACKContext.cpp\
#$(ANON_PARENT)/proxygen/proxygen/lib/http/codec/compress/HPACKEncoder.cpp\
#$(ANON_PARENT)/proxygen/proxygen/lib/http/codec/compress/HPACKDecoder.cpp\
#$(ANON_PARENT)/proxygen/proxygen/lib/http/codec/compress/HPACKEncodeBuffer.cpp\
#$(ANON_PARENT)/proxygen/proxygen/lib/http/codec/compress/HPACKDecodeBuffer.cpp\
#$(ANON_PARENT)/proxygen/proxygen/lib/http/codec/compress/HPACKHeader.cpp\
#$(ANON_PARENT)/proxygen/proxygen/lib/http/codec/compress/HeaderTable.cpp\
#$(ANON_PARENT)/proxygen/proxygen/lib/http/codec/compress/StaticHeaderTable.cpp\
#$(ANON_PARENT)/proxygen/proxygen/lib/http/codec/compress/Huffman.cpp\
#$(ANON_PARENT)/proxygen/proxygen/lib/http/codec/compress/Logging.cpp\
#$(ANON_PARENT)/proxygen/proxygen/fbthrift/thrift/folly/folly/io/IOBuf.cpp\
#$(ANON_PARENT)/proxygen/proxygen/fbthrift/thrift/folly/folly/Malloc.cpp\
#$(ANON_PARENT)/proxygen/proxygen/fbthrift/thrift/folly/folly/SpookyHashV2.cpp\
#$(ANON_PARENT)/proxygen/proxygen/fbthrift/thrift/folly/folly/io/IOBufQueue.cpp

INC_DIRS+=\
$(ANON_ROOT)/src/cpp\
$(ANON_PARENT)/http-parser\
#$(ANON_PARENT)/proxygen

