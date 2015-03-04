
parent_dir=$(patsubst %/,%,$(dir $(patsubst %.,%../foo,$(patsubst %..,%../../foo,$(1)))))

THIS_MAKE := $(lastword $(MAKEFILE_LIST))
ANON_ROOT := $(call parent_dir,$(call parent_dir,$(call parent_dir,$(THIS_MAKE))))
ANON_PARENT := $(patsubst ./..%,..%,$(ANON_ROOT)/..)
ANON_PARENT := $(patsubst ../anon/..,..,$(ANON_PARENT))

include $(ANON_ROOT)/examples/teflon/teflon.mk

CFLAGS_$(ANON_ROOT)/examples/teflon/main.cpp:=-DTEFLON_SERVER_APP


SOURCES+=\
$(ANON_ROOT)/examples/rez/server_main.cpp


RESOURCES_rez=$(wildcard $(ANON_ROOT)/examples/rez/*.html)\
$(wildcard $(ANON_ROOT)/examples/rez/*.css)\
$(wildcard $(ANON_ROOT)/examples/rez/*.js)

RESOURCE_APPS+=rez

$(foreach rez,$(RESOURCES_rez),$(eval $(rez)_REZ_ROOT=$(ANON_ROOT)/examples))

$(foreach rez,$(wildcard $(ANON_ROOT)/examples/rez/*.html),$(eval $(rez)_REZ_TYPE=text/html; charset=UTF-8))

$(foreach rez,$(wildcard $(ANON_ROOT)/examples/rez/*.css),$(eval $(rez)_REZ_TYPE=text/css; charset=UTF-8))

$(foreach rez,$(wildcard $(ANON_ROOT)/examples/rez/*.js),$(eval $(rez)_REZ_TYPE=application/javascript; charset=UTF-8))

