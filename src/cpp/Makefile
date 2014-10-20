.PHONY: all
all:

all: ../../deploy/anon

../../deploy/anon: ../../obj/cpp/main.o
	mkdir -p ../../deploy
	gcc -o ../../deploy/anon ../../obj/cpp/main.o -lgcc -lstdc++

../../obj/cpp/main.o: main.cpp
	mkdir -p ../../obj/cpp
	gcc -c -o ../../obj/cpp/main.o -std=c++11 main.cpp

clean:
	rm -rf ../../deploy ../../obj
