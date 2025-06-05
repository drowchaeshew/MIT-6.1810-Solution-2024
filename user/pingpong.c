#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  int pid; 
  uchar buff[2];

  int p2c[2]; pipe(p2c);
  int c2p[2]; pipe(c2p);

  if ((pid = fork()) == 0) { 
    // child process 
    close(p2c[1]); close(c2p[0]);
    read(p2c[0], buff, 1);
    printf("%d: received ping\n", getpid());
  } else {
    // parent process
    close(p2c[0]); close(c2p[1]);
    write(p2c[1], "/", 1);
    read(c2p[0], buff, 1);
    printf("%d: received pong\n", getpid());
  }
  exit(0);
}
