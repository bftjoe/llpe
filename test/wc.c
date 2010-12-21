
#include <errno.h>
#include <string.h>
#include <stdio.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "bufsize.h"

int main(int argc, char** argv) {

  int fd = open("/tmp/chars", O_RDONLY);
  char buf[BUFSIZE];
  int this_read;
  int newlines = 0;
  while((this_read = read(fd, buf, BUFSIZE)) > 0) {
    for(int i = 0; i < this_read; i++)
      if(buf[i] == '\n')
	newlines++;
  }

  if(this_read == -1) {
    printf("Failed to read: %s\n", strerror(errno));
    return 1;
  }
  else {
    printf("%d lines\n", newlines);
    return 0;
  }
 
}
