# makefile to create the data in the "secrets" and "certs" directories.
# this data consists of private and public PKI key information
# as well as PEM cert files with those keys, as well as some
# password values.  Any time that the rules of this makefile
# _produce_ these values, they will be automatically generated
# based on random values.

print_secrets=@printf "%-10s %-25s %s\n" "creating" $(1): $(2)

secrets:
	@mkdir secrets
	
certs:
	@mkdir certs
	
secrets/raw_ca_password: | secrets
	$(call print_secrets,"random ca password",$@)
	@od --read-bytes 12 -vt x1 -An /dev/urandom | tr -d ' ' | tr -d '\n' > secrets/raw_ca_password

secrets/ca_key.pem: secrets/raw_ca_password
	$(call print_secrets,"ca private key",$@)
	@openssl genrsa -passout pass:`cat secrets/raw_ca_password` -out secrets/ca_key.pem 2048 2> /dev/null

secrets/ca_cert.pem: secrets/ca_key.pem
	$(call print_secrets,"self-signed root ca cert",$@)
	@openssl req -new -x509 -key secrets/ca_key.pem -out secrets/ca_cert.pem -days 1095 -config cert_info/cert_config -batch
	


secrets/raw_ims_password: | secrets
	$(call print_secrets,"random ims password",$@)
	@od --read-bytes 12 -vt x1 -An /dev/urandom | tr -d ' ' | tr -d '\n' > secrets/raw_ims_password

secrets/ims_key.pem: secrets/raw_ims_password
	$(call print_secrets,"ims private key",$@)
	@openssl genrsa -passout pass:`cat secrets/raw_ims_password` -out secrets/ims_key.pem 2048 2> /dev/null

# generate the request (openssl req) and then sign it with our root
# (openssl ca) but, because the ca tool is intended to maintain a database
# of previously signed requests, we blow away any accidentally left over
# dirs and files from that.
secrets/ims_cert.pem: secrets/ca_cert.pem secrets/ims_key.pem
	$(call print_secrets,"ims cert request",$@)
	@rm -f secrets/ims_cert.csr
	@rm -rf obj/auto_gen/certs
	@openssl req -new -key secrets/ims_key.pem -out secrets/ims_cert.csr -config cert_info/cert_config -batch
	@mkdir -p obj/auto_gen/certs/newcerts
	@echo 00 > obj/auto_gen/certs/serial
	@touch obj/auto_gen/certs/index.txt
	@openssl ca -in secrets/ims_cert.csr -out secrets/ims_cert.pem -config cert_info/ca_config -notext -batch 2> /dev/null
	@rm secrets/ims_cert.csr
	@rm -rf obj/auto_gen/certs
	
	
certs/trusted_roots.pem: secrets/ca_cert.pem | certs
	$(call print_secrets,"\"trusted\" public key",$@)
	@openssl rsa -in secrets/ca_key.pem -pubout > certs/trusted_roots.pem 2> /dev/null
	

secrets/passwords.h: secrets/raw_ims_password secrets/raw_ims_password
	$(call print_secrets,"c++ password .h file",$@)
	@echo "#pragma once" > secrets/passwords.h
	@printf "#define IMS_ROOT_CERT_PASSWORD \"" >> secrets/passwords.h
	@cat secrets/raw_ims_password >> secrets/passwords.h
	@printf "\"\n" >> secrets/passwords.h
	@printf "#define IMS_CA_PASSWORD \"" >> secrets/passwords.h
	@cat secrets/raw_ims_password >> secrets/passwords.h
	@printf "\"\n" >> secrets/passwords.h
	
SECRET_FILES=\
secrets/passwords.h\
secrets/ims_cert.pem\
secrets/ca_cert.pem\
certs/trusted_roots.pem

.PHONY: all_certs
all_certs: $(SECRET_FILES)

