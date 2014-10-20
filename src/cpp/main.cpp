
#include <stdio.h>
#include <thread>
#include "log.h"

extern "C" int main(int argc, char** argv)
{
	printf("hello from main, argv[0] = %s!\n", argv[0]);
  anon_log("this is a log line");
  anon_log("this is a compond log line with " << argc << " as the argc value");
	return 0;
}

