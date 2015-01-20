
.PHONY: all clean debug release
all:

#
# set CONFIG for the convenience targets 'debug' and 'run_debug_server'
# we don't need to worry about either of the release targets because
# the default CONFIG is release
#
ifeq (debug,$(MAKECMDGOALS))
 CONFIG=debug
endif
ifeq (run_debug_server,$(MAKECMDGOALS))
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
CFLAGS=$(cflags) -DANON_LOG_FIBER_IDS -DANON_LOG_NET_TRAFFIC=0 -DANON_RUNTIME_CHECKS

#
# these two phony targets depend on (and so build) 'all'
#
debug: all
release: all

anon.INTERMEDIATE_DIR=obj
anon.OUT_DIR=deploy
LIBS=-lgcc -lstdc++ -lpthread -lssl -lcrypto -lanl -lglog -lrt

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

include examples/teflon/teflon.mk

teflon_SOURCES:=$(SOURCES)
SOURCES:=

include scripts/build/anonrules.mk

$(eval $(call anon.BUILD_RULES,test,$(test_SOURCES),$(sort $(INC_DIRS)),$(LIBS)))

$(eval $(call anon.BUILD_RULES,echo,$(echo_SOURCES),$(sort $(INC_DIRS)),$(LIBS)))

$(eval $(call anon.BUILD_RULES,big_client,$(big_client_SOURCES),$(sort $(INC_DIRS)),$(LIBS)))

$(eval $(call anon.BUILD_RULES,epoxy,$(epoxy_SOURCES),$(sort $(INC_DIRS)),$(LIBS)))

$(eval $(call anon.BUILD_RULES,teflon,$(teflon_SOURCES),$(sort $(INC_DIRS)),$(LIBS)))

.PHONY: clean
clean:
	rm -rf $(anon.INTERMEDIATE_DIR) $(anon.OUT_DIR)

