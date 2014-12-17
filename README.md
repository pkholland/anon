see 
http://pkholland.github.io/anon

Basic Build Instructions:
1)  On an Ubuntu 14.04 VM instance:
2)  if not already:
     sudo apt-get install git
     sudo apt-get install g++
3)  create a working directory and cd into it
4)  git clone https://github.com/pkholland/anon.git
5)  git clone https://github.com/joyent/http-parser.git
6)  git clone https://github.com/facebook/proxygen.git
7)  cd proxygen/proxygen
8)  ./deps.sh
       note, as of this writing step 8) eventually hangs in some of the fb testing code, kill it
9)  ./reinstall.sh
10) cd ../../anon
11) make

