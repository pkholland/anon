
#
# the directory that this makefile (anonrules.mk) is in -- relative to the orignal
# directory in which make was invoked $(CURDIR)
#
anon.this_make_dir:=$(dir $(lastword $(MAKEFILE_LIST)))

#
# Root directory into which temp .o files will be placed
#
ifeq (,$(anon.INTERMEDIATE_DIR))
 anon.INTERMEDIATE_DIR:=obj
 $(info anon.INTERMEDIATE_DIR not set, so defaulting to $(CURDIR)/$(anon.INTERMEDIATE_DIR))
 $(info To remove this message set anon.INTERMEDIATE_DIR = something prior to including anonrules.mk)
endif

#
# Root directory into which final executables will be placed
#
ifeq (,$(anon.OUT_DIR))
 anon.OUT_DIR:=out
 $(info anon.OUT_DIR not set, so defaulting to $(CURDIR)/$(anon.OUT_DIR))
 $(info To remove this message set anon.OUT_DIR = something prior to including anonrules.mk)
endif

#
# Default configuration is 'release'
#
CONFIG?=release

#
# default value for "verbose output" is off
#
V?=0

#
# a few tools that we are silent about when verbose is off
#
ifneq (0,$(V))
 anon.mkdir=mkdir
 anon.touch=touch
else
 anon.mkdir=@mkdir
 anon.touch=@touch
endif

#
# Strategy for only calling mkdir on output directories once.
# We put a file named "dir.stamp" in each of those directories
# and then put an "order only" prerequesite to this file in the
# list of each file to compile.
#
%dir.stamp :
	$(anon.mkdir) -p $(@D)
	$(anon.touch) $@

#
# Convert a source path to an object file path.
#
# $1 = Source Name
#
anon.src_to_obj=$(anon.INTERMEDIATE_DIR)/$(CONFIG)/$(basename $(patsubst ./%,%,$(subst ..,__,$(1)))).o

#
# Convert a source path to a dependency file path.
#
# $1 = Source Name
#
anon.src_to_dep=$(anon.INTERMEDIATE_DIR)/$(CONFIG)/$(basename $(patsubst ./%,%,$(subst ..,__,$(1)))).d
#
# Convert a (re)source path to it's equivalent auto_gen file
#
anon.resrc_to_auto_gen=$(anon.INTERMEDIATE_DIR)/auto_gen/$(patsubst ./%,%,$(subst ..,__,$(1))).cpp

#
# Tool to either display short-form (when verbose is off) or
# full command line (when verbose is on) for some tool being
# run as part of a recipe.
#
# $1 = (fully path-qualified, if necessary) tool to run
# $2 = arguments passed to that tool
# $3 = short display-form of the arguments -- frequently just the target file name
#
ifneq (0,$(V))
 anon.CALL_TOOL=$(1) $(2)
else
 anon.CALL_TOOL=@printf "%-10s %s\n" $(notdir $(1)) $(3) && $(1) $(2)
endif

#
# compiler flags that are used in release and debug builds
#
CFLAGS_release+=-O2 -DNDEBUG
CFLAGS_debug+=-g -O0 -DDEBUG

ifneq (clean,$(MAKECMDGOALS))
#
# helper function to compare current options to previously used options
#
#	$1 file name
#	$2 options
#
# Basic idea is to echo the current options, $(2), into the file $(1).  Then compare
# the contents of $(1) with the existing $(1).opts file.  If $(1).opts is missing or is
# not the same as $(1) then replace $(1).opts with $(1).  This has the effect of updating
# the time stamp on $(1).opts whenever the options change, making $(1).opts suitable for a
# prerequisite file to something that uses the options in $2
#
anon.d:=$(anon.INTERMEDIATE_DIR)/$(CONFIG)
anon.options_check=\
mkdir -p $(anon.d);\
echo \"$(subst ",\",$(2))\" > $(anon.d)/$(1);\
if [ -a $(anon.d)/$(1).opts ]; then\
	if [ \"\`diff $(anon.d)/$(1).opts $(anon.d)/$(1)\`\" != \"\" ]; then\
		echo \"updating   $(anon.INTERMEDIATE_DIR)/$(CONFIG)/$(1).opts\";\
		mv $(anon.d)/$(1) $(anon.d)/$(1).opts;\
	else\
		rm $(anon.d)/$(1);\
	fi;\
else\
	echo \"creating   $(anon.INTERMEDIATE_DIR)/$(CONFIG)/$(1).opts\";\
	mv $(anon.d)/$(1) $(anon.d)/$(1).opts;\
fi


#
# execute this shell code when this makefile is parsed.  We can't really do the
# option compare logic as part of a prerequisite of some target because we want to always
# perform the 'diff' operation, which would mean we would need to do something like
# make the target a .PHONY.  But then that would cause make to think it always needed
# to rebuild the target - even if our anon.options_check logic didn't end up updating
# the compiler.opts file.
#
anon.m:=$(shell bash -c "$(call anon.options_check,compiler,$(CFLAGS) $(CFLAGS_$(CONFIG)))")
ifneq (,$(anon.m))
$(info $(anon.m))
endif
anon.m:=$(shell bash -c "$(call anon.options_check,linker,$(LDFLAGS) $(LDFLAGS_$(CONFIG)))")
ifneq (,$(anon.m))
$(info $(anon.m))
endif

# end of "if MAKECMDGOALS != clean"
endif

#
# helper target to see what the compilers and options are set to
#
.PHONY: display_opts
display_opts:
	@echo
	@echo "**** C/C++ compiler options ****"
	@cat $(anon.INTERMEDIATE_DIR)/$(CONFIG)/compiler.opts
	@echo
	@echo "**** linker options ****"
	@cat $(anon.INTERMEDIATE_DIR)/$(CONFIG)/linker.opts
	@echo

#
# helper target to see some interesting targets you can pass to make
#
.PHONY: help
help:
	@cat $(anon.this_make_dir)make.help

#
# tools
#
anon.cc=gcc
anon.cxx=gcc
anon.link=gcc


#
# Compile Macros
#
#	$1 = Souce Name
#	$2 = (with -I prefix) include directories
#
define anon.c_compile_rule

-include $(call anon.src_to_dep,$(1))

$(call anon.src_to_obj,$(1)): $(1) $(anon.INTERMEDIATE_DIR)/$(CONFIG)/compiler.opts | $(dir $(call anon.src_to_obj,$(1)))dir.stamp
	$(call anon.CALL_TOOL,$(anon.cc),-o $$@ -c $$< -MD -MF $(call anon.src_to_dep,$(1)) $(2) $(CFLAGS) $(CFLAGS_$(CONFIG)) $(CFLAGS_$(1)),$$@)

endef

define anon.cxx_compile_rule

-include $(call anon.src_to_dep,$(1))

$(call anon.src_to_obj,$(1)): $(1) $(anon.INTERMEDIATE_DIR)/$(CONFIG)/compiler.opts | $(dir $(call anon.src_to_obj,$(1)))dir.stamp
	$(call anon.CALL_TOOL,$(anon.cxx),-o $$@ -c $$< -MD -MF $(call anon.src_to_dep,$(1)) $(2) -std=c++11 $(CFLAGS) $(CFLAGS_$(CONFIG)) $(CFLAGS_$(1)),$$@)

endef

define anon.linker_rule

$(anon.OUT_DIR)/$(CONFIG)/$(1): $(anon.INTERMEDIATE_DIR)/$(CONFIG)/linker.opts $(foreach src,$(2),$(call anon.src_to_obj,$(src)))
	$(anon.mkdir) -p $$(@D)
	$(call anon.CALL_TOOL,$(anon.link),$(LDFLAGS) $(LDFLAGS_$(CONFIG)) $$(filter-out %.opts,$$^) -o $$@ $(3),$$@)

endef

anon.all_sources:=

#$(info $(filter-out $(anon.all_sources),$(2)))
#anon.new_sources:=$(filter-out $(anon.all_sources),$(2))
#anon.all_sources+=$(filter-out $(anon.all_sources),$(2))

#
# $1 build name
# $2 source files to compile
# $3 include directories
# $4 additional libraries to link to
#
define anon.BUILD_RULES

$(foreach cpp_src,$(filter %.cc %.cpp,$(filter-out $(anon.all_sources),$(2))),$(call anon.cxx_compile_rule,$(cpp_src),$(foreach inc,$(3),-I$(inc))))
$(foreach c_src,$(filter %.c,$(filter-out $(anon.all_sources),$(2))),$(call anon.c_compile_rule,$(c_src),$(foreach inc,$(3),-I$(inc))))
$(call anon.linker_rule,$(1),$(2),$(4))

all: $(anon.OUT_DIR)/$(CONFIG)/$(1)

anon.all_sources+=$(filter-out $(anon.all_sources),$(2))

endef

