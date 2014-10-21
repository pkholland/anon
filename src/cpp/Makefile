.PHONY: all
all:

all: ../../deploy/anon

../../deploy/anon: ../../obj/cpp/main.o ../../obj/cpp/udp_dispatch.o ../../obj/cpp/big_id_crypto.o
	mkdir -p ../../deploy
	gcc -o ../../deploy/anon ../../obj/cpp/main.o ../../obj/cpp/udp_dispatch.o ../../obj/cpp/big_id_crypto.o -lgcc -lstdc++ -lpthread -lssl -lcrypto

../../obj/cpp/main.o: main.cpp
	mkdir -p ../../obj/cpp
	gcc -c -o ../../obj/cpp/main.o -std=c++11 main.cpp

../../obj/cpp/udp_dispatch.o: udp_dispatch.cpp
	mkdir -p ../../obj/cpp
	gcc -c -o ../../obj/cpp/udp_dispatch.o -std=c++11 udp_dispatch.cpp
 
../../obj/cpp/big_id_crypto.o: big_id_crypto.cpp
	mkdir -p ../../obj/cpp
	gcc -c -o ../../obj/cpp/big_id_crypto.o -std=c++11 big_id_crypto.cpp

clean:
	rm -rf ../../deploy ../../obj
