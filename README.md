see 
http://pkholland.github.io/anon

Basic Build Instructions:<br>
1)  On an Ubuntu 14.04 VM instance:<br>
2)  if not already:<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;sudo apt-get install git<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;sudo apt-get install g++<br>
3)  create a working directory and cd into it<br>
4)  git clone https://github.com/pkholland/anon.git<br>
5)  git clone https://github.com/joyent/http-parser.git<br>
6)  git clone https://github.com/facebook/proxygen.git<br>
7)  cd proxygen/proxygen<br>
8)  ./deps.sh<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;note, as of this writing step 8) eventually hangs in some of the fb testing code, kill it<br>
9)  ./reinstall.sh<br>
10) cd ../../anon<br>
11) make<br>

Running the test code:<br>
./deploy/release/test<br>

