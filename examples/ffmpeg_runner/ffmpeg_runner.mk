parent_dir=$(patsubst %/,%,$(dir $(patsubst %.,%../foo,$(patsubst %..,%../../foo,$1))))

THIS_MAKE := $(lastword $(MAKEFILE_LIST))
ANON_ROOT := $(call parent_dir,$(call parent_dir,$(call parent_dir,$(THIS_MAKE))))
ANON_PARENT := $(patsubst ./..%,..%,$(ANON_ROOT)/..)
ANON_PARENT := $(patsubst ../anon/..,..,$(ANON_PARENT))

SOURCES+=\
$(ANON_ROOT)/examples/ffmpeg_runner/main.cpp\
$(ANON_ROOT)/examples/resin/worker_message.proto\
$(ANON_ROOT)/src/cpp/fiber.cpp\
$(ANON_ROOT)/src/cpp/io_dispatch.cpp

INC_DIRS+=$(ANON_ROOT)/src/cpp $(ANON_ROOT)/examples/resin
LIBS:=-lprotobuf-lite $(LIBS)
