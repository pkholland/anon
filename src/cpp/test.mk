
parent_dir=$(patsubst %/,%,$(dir $(patsubst %.,%../foo,$(patsubst %..,%../../foo,$(1)))))

THIS_MAKE := $(lastword $(MAKEFILE_LIST))
ANON_ROOT := $(call parent_dir,$(call parent_dir,$(call parent_dir,$(THIS_MAKE))))

SOURCES+=\
$(ANON_ROOT)/src/cpp/main.cpp\
$(ANON_ROOT)/src/cpp/io_dispatch.cpp\
$(ANON_ROOT)/src/cpp/udp_dispatch.cpp\
$(ANON_ROOT)/src/cpp/big_id_crypto.cpp

INC_DIRS+=\
$(ANON_ROOT)

