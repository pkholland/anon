
.PHONY: all clean debug release
all:

#
# set CONFIG for the convenience target 'debug'
# we don't need to worry about any of the release targets because
# the default CONFIG is release
#
ifeq (debug,$(MAKECMDGOALS))
 CONFIG=debug
endif

#
# ANON_LOG_FIBER_IDS
#   if defined, causes log lines to contain both the thread id
#   as well as the fiber id - or "....." if the code calling the
#   logging function is not running on a fiber
#
# ANON_LOG_NET_TRAFFIC:
#   0 - (or undefined) no logging networking activity
#   1 - log errors from sys calls like 'socket' and 'recv'
#   2 - 1) plus, log errors in html,etc... formatting sent from clients
#   3 - 2) plus, general logging of what ips are connecting
CFLAGS=$(cflags) -DANON_LOG_FIBER_IDS -DANON_LOG_NET_TRAFFIC=1 -DxANON_RUNTIME_CHECKS

ifeq (1,$(ASAN))
 CFLAGS+=-fsanitize=address -fno-omit-frame-pointer
 LDFLAGS+=-fsanitize=address
endif

#
# these two phony targets depend on (and so build) 'all'
#
debug: all
release: all

anon.INTERMEDIATE_DIR=obj
anon.OUT_DIR=deploy
LIBS=-lstdc++ -lpthread -lssl -lcrypto -lanl -lrt

include examples/test/test.mk

test_SOURCES:=$(SOURCES)
SOURCES:=

include examples/echo/echo.mk

echo_SOURCES:=$(SOURCES)
SOURCES:=

include examples/big_client/big_client.mk

big_client_SOURCES:=$(SOURCES)
SOURCES:=

include examples/epoxy/epoxy.mk

epoxy_SOURCES:=$(SOURCES)
SOURCES:=

ifneq ($(ANON_AWS),)
include examples/resin/resin.mk

resin_SOURCES:=$(SOURCES)
SOURCES:=
endif

include examples/teflon_hello/teflon_hello.mk

teflon_SOURCES:=$(SOURCES)
SOURCES:=

include examples/srv_test/srv_test.mk

srv_test_SOURCES:=$(SOURCES)
SOURCES:=

include examples/rez/rez.mk

rez_SOURCES:=$(SOURCES)
SOURCES:=

include examples/ffmpeg_runner/ffmpeg_runner.mk

ffmpeg_runner_SOURCES:=$(SOURCES)
SOURCES:=

include secrets.mk

include scripts/build/anonrules.mk

$(eval $(call anon.BUILD_RULES,test,$(test_SOURCES),$(sort $(INC_DIRS)),$(LIBS)))

$(eval $(call anon.BUILD_RULES,echo,$(echo_SOURCES),$(sort $(INC_DIRS)),$(LIBS)))

$(eval $(call anon.BUILD_RULES,big_client,$(big_client_SOURCES),$(sort $(INC_DIRS)),$(LIBS)))

$(eval $(call anon.BUILD_RULES,epoxy,$(epoxy_SOURCES),$(sort $(INC_DIRS)),$(LIBS)))

ifneq ($(ANON_AWS),)
$(eval $(call anon.BUILD_RULES,resin,$(resin_SOURCES),$(sort $(INC_DIRS)),$(LIBS)))
endif

$(eval $(call anon.BUILD_RULES,teflon_hello,$(teflon_SOURCES),$(sort $(INC_DIRS)),$(LIBS)))

$(eval $(call anon.BUILD_RULES,srv_test,$(srv_test_SOURCES),$(sort $(INC_DIRS)),$(LIBS)))

$(eval $(call anon.BUILD_RULES,rez,$(rez_SOURCES),$(sort $(INC_DIRS)),$(LIBS)))

$(eval $(call anon.BUILD_RULES,ffmpeg_runner,$(ffmpeg_runner_SOURCES),$(sort $(INC_DIRS)),$(LIBS)))

all: $(SECRET_FILES)

.PHONY: clean
clean:
	rm -rf $(anon.INTERMEDIATE_DIR) $(anon.OUT_DIR) secrets certs

