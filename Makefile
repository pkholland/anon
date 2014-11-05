
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
CFLAGS=$(cflags) -DANON_LOG_FIBER_IDS -DANON_LOG_NET_TRAFFIC=2

#
# these two phony targets depend on (and so build) 'all'
#
debug: all
release: all

anon.INTERMEDIATE_DIR=obj
anon.OUT_DIR=deploy
LIBS=-lgcc -lstdc++ -lpthread -lssl -lcrypto -lanl

include src/cpp/test.mk
include scripts/build/anonrules.mk

$(eval $(call anon.BUILD_RULES,test,$(sort $(SOURCES)),$(INC_DIRS),$(LIBS)))

.PHONY: clean
clean:
	rm -rf $(anon.INTERMEDIATE_DIR) $(anon.OUT_DIR)
