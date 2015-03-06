
parent_dir=$(patsubst %/,%,$(dir $(patsubst %.,%../foo,$(patsubst %..,%../../foo,$1))))

THIS_MAKE := $(lastword $(MAKEFILE_LIST))
ANON_ROOT := $(call parent_dir,$(call parent_dir,$(call parent_dir,$(THIS_MAKE))))
ANON_PARENT := $(patsubst ./..%,..%,$(ANON_ROOT)/..)
ANON_PARENT := $(patsubst ../anon/..,..,$(ANON_PARENT))

include $(ANON_ROOT)/examples/teflon/teflon.mk

CFLAGS_$(ANON_ROOT)/examples/teflon/main.cpp:=-DTEFLON_SERVER_APP


SOURCES+=\
$(ANON_ROOT)/examples/rez/server_main.cpp

#
# simple example of just including all of the following files
# as resources
#
rez_html_resources=$(wildcard $(ANON_ROOT)/examples/rez/*.html)
rez_css_resources=$(wildcard $(ANON_ROOT)/examples/rez/*.css)
rez_js_resources=$(wildcard $(ANON_ROOT)/examples/rez/*.js)

RESOURCES_rez=$(rez_html_resources)\
$(rez_css_resources)\
$(rez_js_resources)

#
# register 'rez' as an app that will need resources
#
RESOURCE_APPS+=rez

#
# specify that all of our resources will be anchored at "resources"
# the rule is to strip off the "_REZ_HOST_ROOT" part of the path
# and replace it with the "_REZ_SRV_ROOT".  In this case we do the
# same replacement for each file, causing all of them to show up
# directly inside "/resources"
#
$(foreach rez,$(RESOURCES_rez),$(eval $(rez)_REZ_HOST_ROOT=$(ANON_ROOT)/examples/rez))
$(foreach rez,$(RESOURCES_rez),$(eval $(rez)_REZ_SRV_ROOT=/resources))


#
# set the mime type for each of the resources (in this case, just based on the file name extension)
#
$(foreach rez,$(rez_html_resources),$(eval $(rez)_REZ_TYPE=text/html; charset=UTF-8))
$(foreach rez,$(rez_css_resources),$(eval $(rez)_REZ_TYPE=text/css; charset=UTF-8))
$(foreach rez,$(rez_js_resources),$(eval $(rez)_REZ_TYPE=application/javascript; charset=UTF-8))

