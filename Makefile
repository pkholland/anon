
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

CFLAGS=$(cflags) -DANON_LOG_FIBER_IDS

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

$(eval $(call anon.BUILD_RULES,test,$(sort $(SOURCES)),$(INCLUDE),$(LIBS)))

.PHONY: clean
clean:
	rm -rf $(anon.INTERMEDIATE_DIR) $(anon.OUT_DIR)
