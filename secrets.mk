# makefile to create the data in the "secrets" and "certs" directories.
# this data consists of private and public PKI key information
# as well as PEM cert files with those keys, as well as some
# password values.  Any time that the rules of this makefile
# _produce_ these values, they will be automatically generated
# based on random values.  Several of the default behaviors are
# specified in the files ca_config and cert_config for generating
# root certificate and a server certificate, signed by that root.
# One of the things this set of make rules produces is the file
# certs/trusted_roots.pem.  If you import that pem file into the
# set of trusted certificate authorities that your web browser understands,
# you can then use the secrets/anon_cert.pem and secrets/anon_key.pem
# files in an openssl-based server session, allowing your browser
# to go to "https://localhost:<whatever port number>" and be able
# to validate the connection and certificate chain.  See the
# "epoxy" and "teflon" example projects in anon for an example
# of how to do this.
#
# Example recipe:
#
#   1) cd anon
#   2) make
#   3) ./deploy/release/epoxy 1768 teflon
#
# with that running, in a second terminal window:
#
#   4) cd anon/deploy/release
#   5) echo "start teflon" > .epoxy_cmd && cat .epoxy_cmd
#
# At this point the teflon web server should be running https on port 1768
#
# To verify directly with openssl do:
#
#   6) openssl s_client -connect localhost:1768 -CAfile ../../certs/trusted_roots.pem
#
# The last line of openssl's output should look like:
#
#     Verify return code: 0 (ok)
#
# To verify with Firefox do:
#
#   a) launch firefox
#   b) select the menu File > Preferences
#   c) select the "Advanced" tab
#   d) click the "View Certificates" button
#   e) click the "Import..." button
#   f) navigate to anon/certs and select the trusted_roots.pem file
#   g) click "Open"
#   h) select the "Trust this CA to identify websites" check box
#   i) click "OK"
#   j) click "OK"
#
# then in firefox navitage to https://localhost:1768.  If all goes well
# it will bring up the teflon page without any security complaints.
#
# To remove the certificate from Firefox's trusted list, search the
# certificate view window for "ANON Open Development" and Delete its
# "localhost" certificate.

parent_dir=$(patsubst %/,%,$(dir $(patsubst %.,%../foo,$(patsubst %..,%../../foo,$1))))
THIS_MAKE := $(lastword $(MAKEFILE_LIST))
ANON_ROOT := $(call parent_dir,$(THIS_MAKE))

print_secrets=@printf "%-10s %-25s %s\n" "creating" $1: $2

secrets:
	@mkdir secrets
	
certs:
	@mkdir certs
	
secrets/raw_ca_key_password: | secrets
	$(call print_secrets,"random ca key password",$@)
	@od --read-bytes 12 -vt x1 -An /dev/urandom | tr -d ' ' | tr -d '\n' > secrets/raw_ca_key_password

secrets/ca_key.pem: secrets/raw_ca_key_password
	$(call print_secrets,"ca private key",$@)
	@openssl genrsa -des3 -passout file:secrets/raw_ca_key_password -out secrets/ca_key.pem 2048 2> /dev/null

secrets/ca_cert.pem: secrets/ca_key.pem
	$(call print_secrets,"self-signed root ca cert",$@)
	@openssl req -new -x509 -key secrets/ca_key.pem -passin file:secrets/raw_ca_key_password -out secrets/ca_cert.pem -days 1095 -config $(ANON_ROOT)/cert_info/cert_config -batch
	


secrets/raw_srv_key_password: | secrets
	$(call print_secrets,"random srv key password",$@)
	@od --read-bytes 12 -vt x1 -An /dev/urandom | tr -d ' ' | tr -d '\n' > secrets/raw_srv_key_password

secrets/srv_key.pem: secrets/raw_srv_key_password
	$(call print_secrets,"srv private key",$@)
	@openssl genrsa -des3 -passout file:secrets/raw_srv_key_password -out secrets/srv_key.pem 2048 2> /dev/null
	
secrets/raw_srv_cert_password: | secrets
	$(call print_secrets,"random srv cert password",$@)
	@od --read-bytes 12 -vt x1 -An /dev/urandom | tr -d ' ' | tr -d '\n' > secrets/raw_srv_cert_password


# generate the request (openssl req) and then sign it with our root
# (openssl ca) but, because the ca tool is intended to maintain a database
# of previously signed requests, we blow away any accidentally left over
# dirs and files from that.
secrets/srv_cert.pem: secrets/ca_cert.pem secrets/srv_key.pem secrets/raw_srv_cert_password
	$(call print_secrets,"srv cert request",$@)
	@rm -f secrets/srv_cert.csr
	@rm -rf obj/auto_gen/certs
	@openssl req -new -key secrets/srv_key.pem -passin file:secrets/raw_srv_key_password -passout file:secrets/raw_srv_cert_password -out secrets/srv_cert.csr -config $(ANON_ROOT)/cert_info/cert_config -batch
	@mkdir -p obj/auto_gen/certs/newcerts
	@bash -c "printf \"%x\n\" \$$((\$$RANDOM%256))" > obj/auto_gen/certs/serial
	@touch obj/auto_gen/certs/index.txt
	@openssl ca -in secrets/srv_cert.csr -passin file:secrets/raw_ca_key_password -out secrets/srv_cert.pem -config $(ANON_ROOT)/cert_info/ca_config -notext -batch 2> /dev/null
	@rm secrets/srv_cert.csr
	@rm -rf obj/auto_gen/certs
	
	
certs/trusted_roots.pem: secrets/ca_cert.pem | certs
	$(call print_secrets,"\"trusted\" public cert",$@)
	@cp -f secrets/ca_cert.pem certs/trusted_roots.pem
	

secrets/passwords.h: secrets/raw_srv_cert_password secrets/raw_srv_key_password
	$(call print_secrets,"c++ password .h file",$@)
	@echo "#pragma once" > secrets/passwords.h
	@printf "#define ANON_SRV_KEY_PASSWORD \"" >> secrets/passwords.h
	@cat secrets/raw_srv_key_password >> secrets/passwords.h
	@printf "\"\n" >> secrets/passwords.h
	@printf "#define ANON_SRV_CERT_PASSWORD \"" >> secrets/passwords.h
	@cat secrets/raw_srv_cert_password >> secrets/passwords.h
	@printf "\"\n" >> secrets/passwords.h
	
SECRET_FILES=\
secrets/passwords.h\
secrets/srv_cert.pem\
secrets/ca_cert.pem\
certs/trusted_roots.pem

.PHONY: all_certs
all_certs: $(SECRET_FILES)

INC_DIRS+=secrets

$(ANON_ROOT)/src/cpp/tls_context.cpp: secrets/passwords.h

