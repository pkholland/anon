
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
anon.src_to_obj=$(anon.INTERMEDIATE_DIR)/$(CONFIG)/$(basename $(patsubst ./%,%,$(subst ..,__,$1))).o

#
# Convert a source path to a dependency file path.
#
# $1 = Source Name
#
anon.src_to_dep=$(anon.INTERMEDIATE_DIR)/$(CONFIG)/$(basename $(patsubst ./%,%,$(subst ..,__,$1))).d
#
# Convert a (re)source path to it's equivalent auto_gen file
#
anon.resrc_to_auto_gen=$(anon.INTERMEDIATE_DIR)/auto_gen/rez/$(CONFIG)/$(patsubst ./%,%,$(subst ..,__,$1)).cpp

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
 anon.CALL_TOOL=$1 $2
else
 anon.CALL_TOOL=@printf "%-10s %s\n" $(notdir $1) $3 && $1 $2
endif

#
# compiler flags that are used in release and debug builds
#
CFLAGS_release+=-O2 -DNDEBUG
CFLAGS_debug+=-ggdb -O0 -DDEBUG

ifneq (clean,$(MAKECMDGOALS))
#
# helper function to compare current options to previously used options
#
#	$1 file name
#	$2 options
#
# Basic idea is to echo the current options, $2, into the file $1.  Then compare
# the contents of $1 with the existing $1.opts file.  If $1.opts is missing or is
# not the same as $1 then replace $1.opts with $1.  This has the effect of updating
# the time stamp on $1.opts whenever the options change, making $1.opts suitable for a
# prerequisite file to something that uses the options in $2
#
anon.d:=$(anon.INTERMEDIATE_DIR)/$(CONFIG)
anon.options_check=\
mkdir -p $(anon.d);\
echo \"$(subst ",\",$2)\" > $(anon.d)/$1;\
if [ -a $(anon.d)/$1.opts ]; then\
	if [ \"\`diff $(anon.d)/$1.opts $(anon.d)/$1\`\" != \"\" ]; then\
		echo \"updating   $(anon.INTERMEDIATE_DIR)/$(CONFIG)/$1.opts\";\
		mv $(anon.d)/$1 $(anon.d)/$1.opts;\
	else\
		rm $(anon.d)/$1;\
	fi;\
else\
	echo \"creating   $(anon.INTERMEDIATE_DIR)/$(CONFIG)/$1.opts\";\
	mv $(anon.d)/$1 $(anon.d)/$1.opts;\
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
# $3 = additional dependencies (if any)
#
define anon.c_compile_rule

-include $(call anon.src_to_dep,$1)

$(call anon.src_to_obj,$1): $1 $3 $(anon.INTERMEDIATE_DIR)/$(CONFIG)/compiler.opts | $(dir $(call anon.src_to_obj,$1))dir.stamp
	$(call anon.CALL_TOOL,$(anon.cc),-o $$@ -c $$< -MD -MF $(call anon.src_to_dep,$1) $2 $(CFLAGS) $(CFLAGS_$(CONFIG)) $(CFLAGS_$1),$$@)

endef

define anon.cxx_compile_rule

-include $(call anon.src_to_dep,$1)

$(call anon.src_to_obj,$1): $1 $3 $(anon.INTERMEDIATE_DIR)/$(CONFIG)/compiler.opts | $(dir $(call anon.src_to_obj,$1))dir.stamp
	$(call anon.CALL_TOOL,$(anon.cxx),-o $$@ -c $$< -MD -MF $(call anon.src_to_dep,$1) $2 -std=c++11 $(CFLAGS) $(CFLAGS_$(CONFIG)) $(CFLAGS_$1),$$@)

endef

define anon.linker_rule

$(anon.OUT_DIR)/$(CONFIG)/$1: $(anon.INTERMEDIATE_DIR)/$(CONFIG)/linker.opts $(foreach src,$2,$(call anon.src_to_obj,$(src)))
	$(anon.mkdir) -p $$(@D)
	$(call anon.CALL_TOOL,$(anon.link),$(LDFLAGS) $(LDFLAGS_$(CONFIG)) $$(filter-out %.opts,$$^) -o $$@ $3,$$@)

endef

anon.all_sources:=

#$(info $(filter-out $(anon.all_sources),$2))
#anon.new_sources:=$(filter-out $(anon.all_sources),$2)
#anon.all_sources+=$(filter-out $(anon.all_sources),$2)

#
# $1 build name
# $2 source files to compile
# $3 include directories
# $4 additional libraries to link to
# $5 additional dependencies for all source files
#
define anon.BUILD_RULES

$(foreach cpp_src,$(filter %.cc %.cpp,$(filter-out $(anon.all_sources),$2 $(call anon.all_resource_files,$1))),$(call anon.cxx_compile_rule,$(cpp_src),$(foreach inc,$3,-I$(inc)),$5))
$(foreach c_src,$(filter %.c,$(filter-out $(anon.all_sources),$2)),$(call anon.c_compile_rule,$(c_src),$(foreach inc,$3,-I$(inc)),$5))
$(call anon.linker_rule,$1,$2 $(call anon.all_resource_files,$1),$4)

all: $(anon.OUT_DIR)/$(CONFIG)/$1

anon.all_sources+=$(filter-out $(anon.all_sources),$2 $(call anon.all_resource_files,$1))

endef


###############################################################################################
#
#   Resource Handling -- only evaluated if $(RESOURCES) contains at least one file
#
###############################################################################################

RESOURCES:=$(sort $(foreach app,$(RESOURCE_APPS),$(RESOURCES_$(app))))

ifneq (,$(RESOURCES))

#
#	$1 the path/file name to "sanitize" (make it a legal C token)
#
#   rules:
#     X	-> XX
#     .	-> X_
#     / -> XS
#     - -> XD
# 
anon.sanitize_rez_name=rezRec_$(subst -,XD,$(subst /,XS,$(subst .,X_,$(subst X,XX,$1))))


#
# $1 file name of path/file to be treated as a resource.  The recipe for this
# runs the contents of the file, $1, through od (and then sed) to generate
# a text file that contains a C-like array of hex values -- as in "0x54, 0x2f, ..."
# with the contents of the file.
#
define anon.resource_rule

$(call anon.resrc_to_auto_gen,$1): $1 | $(dir $(call anon.resrc_to_auto_gen,$1))dir.stamp
	@echo "converting $$@"
	@echo "// AUTO-GENERATED by anonrules.mk, based on the contents of $1" > $$@
	@echo "// DO NOT EDIT" >> $$@
	@echo "" >> $$@
	@echo "#include \"resources.h\"" >> $$@
	@echo "#include <map>" >> $$@
	@echo "" >> $$@
	@echo "namespace {" >> $$@
	@echo "const unsigned char uncompressed[] = {" >> $$@
	@cat $$< | od -vt x1 -An | sed 's/\(\ \)\([0-9a-f][0-9a-f]\)/0x\2,/g' >> $$@
	@echo "};" >> $$@
	@echo "const unsigned char compressed[] = {" >> $$@
	@cat $$< | gzip - | od -vt x1 -An | sed 's/\(\ \)\([0-9a-f][0-9a-f]\)/0x\2,/g' >> $$@
	@echo "};" >> $$@
	@printf "const char* etag = \"\\\"%s\\\"\";\n" `sha1sum $1 | sed -r 's/([^ ]+).*/\1/g'` >> $$@
	@printf "const rez_file_ent ent = { &uncompressed[0], %d, &compressed[0], %d, etag, $3 };\n" `cat $$< | wc -c` `cat $$< | gzip - | wc -c` >> $$@
	@echo "}" >> $$@
	@echo "void $(call anon.sanitize_rez_name,$1)(std::map<std::string, const rez_file_ent*>& mp);" >> $$@
	@echo "void $(call anon.sanitize_rez_name,$1)(std::map<std::string, const rez_file_ent*>& mp) { mp[\"$2\"] = &ent; }" >> $$@
	@echo "" >> $$@

endef

define anon.rez_path
$(if $($1_REZ_SRV_ROOT),$(patsubst $($1_REZ_HOST_ROOT)/%,$($1_REZ_SRV_ROOT)/%,$1),$1)
endef

define anon.content_type
$(if $($1_REZ_TYPE),\"$($1_REZ_TYPE)\",\"text/plain; charset=UTF-8\")
endef

#
# establish build targets of all of the resource C++ files
#
$(foreach rez,$(RESOURCES),$(eval $(call anon.resource_rule,$(rez),$(call anon.rez_path,$(rez)),$(call anon.content_type,$(rez)))))


define anon.resources_rule

$(call anon.resrc_to_auto_gen,$1): $2
	@echo "// AUTO-GENERATED by anonrules.mk" > $$@
	@echo "// DO NOT EDIT" >> $$@
	@echo "" >> $$@
	@echo "#include \"resources.h\"" >> $$@
	@echo "#include <map>" >> $$@
	@echo "" >> $$@
	@echo "$3" | sed 's/; /;\n/g' >> $$@
	@echo "namespace {" >> $$@
	@echo "class Rez" >> $$@
	@echo "{" >> $$@
	@echo "public:" >> $$@
	@echo "  Rez()" >> $$@
	@echo "  {" >> $$@
	@echo "    $4" | sed 's/; /;\n    /g' >> $$@
	@echo "  }" >> $$@
	@echo "  std::map<std::string, const rez_file_ent*> mp;" >> $$@
	@echo "};" >> $$@
	@echo "Rez rz;" >> $$@
	@echo "}" >> $$@
	@echo "const rez_file_ent* get_resource(const std::string& path)" >> $$@
	@echo "{" >> $$@
	@echo "  auto it = rz.mp.find(path);" >> $$@
	@echo "  return it == rz.mp.end() ? 0 : it->second;" >> $$@
	@echo "}" >> $$@
	@echo "" >> $$@
	@echo "void do_for_each_rez(for_each_rez_helper* h)" >> $$@
	@echo "{" >> $$@
	@echo "  for (auto it = rz.mp.begin(); it != rz.mp.end(); it++)" >> $$@
	@echo "    h->rez(it->first, it->second);" >> $$@
	@echo "  delete h;" >> $$@
	@echo "}" >> $$@
	@echo "" >> $$@

endef

#
# given a list of resource files in $1, compute the equivalent list
# of resrc_to_auto_gen files
#
anon.resource_files=$(foreach file,$(RESOURCES_$1),$(call anon.resrc_to_auto_gen,$(file)))

#
# the list of additional files an app, $1, will need to compile
#
anon.all_resource_files=$(if $(RESOURCES_$1),$(call anon.resrc_to_auto_gen,$1) $(call anon.resource_files,$1))

#
# C++ snippets to declare, and call all of the resource initialzation functions
#
anon.decl_initializers=$(foreach file,$(RESOURCES_$1),void $(call anon.sanitize_rez_name,$(file))(std::map<std::string, const rez_file_ent*>& mp);)
anon.call_initializers=$(foreach file,$(RESOURCES_$1),$(call anon.sanitize_rez_name,$(file))(mp);)

#
# instantiate a build dependency rule for each of the main resource source files
# this file will implement the get_resource (and do_for_each_rez) functions for 
# the particular app.
#
$(foreach app,$(RESOURCE_APPS),$(eval $(call anon.resources_rule,$(app),$(call anon.resource_files,$(app)),$(call anon.decl_initializers,$(app)),$(call anon.call_initializers,$(app)))))

#####################################


define anon.display_resources

.PHONY: display_rez_$1
display_rez_$1:
	@echo ""
	@echo "resource list for application \"$1\""
	@printf "  %-50s   %-50s   %s\n" $2
	
endef

anon.rez_display="src file:" "server resource path:" "mime type:" $(foreach rez,$(RESOURCES_$1),$(rez) $(call anon.rez_path,$(rez)) "$(call anon.content_type,$(rez))")

$(foreach app,$(RESOURCE_APPS),$(eval $(call anon.display_resources,$(app),$(call anon.rez_display,$(app)))))

.PHONY: rez_explain
rez_explain:
	@echo ""
	@echo "For each \"application\" listed below you will see all"
	@echo "resources contained in that application.  Each resource"
	@echo "shows the source file of that resource under \"src file\","
	@echo "the path/location that this file will be available on the"
	@echo "server under \"server resource path\", and the mime type that"
	@echo "will be reported for that resource.  These files, their server"
	@echo "paths, and mime types are all controled by setting make"
	@echo "variables.  See anon's examples/rez project for details."

.PHONY: display_rez
display_rez: rez_explain $(foreach app,$(RESOURCE_APPS),display_rez_$(app))
	@echo ""

endif

###############################################################################################
#
#   End of Resource Handling
#
###############################################################################################










