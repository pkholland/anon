see 
http://pkholland.github.io/anon

Basic Build Instructions:<br>
1)  On an Ubuntu 14.04 VM instance:<br>
2)  if not already:<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;sudo apt-get install git<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;sudo apt-get install g++<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;sudo apt-get install libssl-dev<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;sudo apt-get install memcached<br>
3)  create a working directory and cd into it<br>
4)  git clone https://github.com/pkholland/anon.git<br>
5)  git clone https://github.com/joyent/http-parser.git<br>
<b>skip</b> 6)  git clone https://github.com/facebook/proxygen.git<br>
<b>skip</b> 7)  cd proxygen/proxygen<br>
<b>skip</b> 8)  ./deps.sh<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;note, as of this writing step 8) eventually hangs in some of the fb testing code, kill it<br>
<b>skip</b> 9)  ./reinstall.sh<br>
10) cd &lt;working dir&gt;/anon<br>
11) make<br>

Running the test code:<br>
./deploy/release/test<br>
&nbsp;&nbsp;then type "h"&lt;return&gt; for a list of available tests

-- Currently the proxygen-related pieces are not being used and you don't need to follow those steps (6-9) to get anon building and running.

Note that the install of memcached is only for running some of the test code, it is not required for simply building anon.
This default install of memcaced configures you machine to run memcached as a daemon.

Config/Build in structions for the Raspberry Pi 2:<br>
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

