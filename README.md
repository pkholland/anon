see 
http://pkholland.github.io/anon

### Basic Build Instructions:
1)  On an Ubuntu 14.04 VM instance:
2)  if not already:
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;sudo apt-get install git\
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;sudo apt-get install g++\
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;sudo apt-get install libssl-dev\
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;sudo apt-get install memcached
3)  create a working directory and cd into it
4)  git clone https://github.com/pkholland/anon.git
5)  git clone https://github.com/joyent/http-parser.git<br>
<b>skip</b>  git clone https://github.com/facebook/proxygen.git<br>
<b>skip</b>  cd proxygen/proxygen<br>
<b>skip</b>  ./deps.sh<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;note, as of this writing step 8) eventually hangs in some of the fb testing code, kill it<br>
<b>skip</b>  ./reinstall.sh
10) cd &lt;working dir&gt;/anon
11) make

Running the test code:\
./deploy/release/test\
&nbsp;&nbsp;then type "h"&lt;return&gt; for a list of available tests

-- Currently the proxygen-related pieces are not being used and you don't need to follow those steps (6-9) to get anon building and running.

Note that the install of memcached is only for running some of the test code, it is not required for simply building anon.
This default install of memcaced configures you machine to run memcached as a daemon.

###Config/Build in structions for the Raspberry Pi 2:<br>
- boot your Raspberry as normal, get it connected to the internet and then in a command shell
- sudo apt-get update
- sudo apt-get dist-upgrade
- sudo apt-get install gcc-4.8
- sudo apt-get install g++-4.8
- cd /usr/bin
- optional, but do before the next step: sudo apt-get remove gcc-4.6
- sudo ln -s -f gcc-4.8 gcc
- in a text editor, sudo open /etc/modules and add "ipv6" as a last line
- reboot your Pi
- follow git/build instructions from above

