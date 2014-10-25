
.PHONY: all
all:

anon.INTERMEDIATE_DIR=obj
anon.OUT_DIR=deploy
LIBS=-lgcc -lstdc++ -lpthread -lssl -lcrypto

include src/cpp/test.mk
include scripts/build/anonrules.mk

$(eval $(call anon.BUILD_RULES,test,$(sort $(SOURCES)),$(INCLUDE),$(LIBS)))

.PHONY: clean
clean:
	rm -rf $(anon.INTERMEDIATE_DIR) $(anon.OUT_DIR)
