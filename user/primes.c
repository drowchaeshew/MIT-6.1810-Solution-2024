#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

#define READ 0 
#define WRITE 1 
#define MAX_SIZE 280

int drops(int left) {

  int prime, curr;
  read(left, &prime, sizeof(int));
  printf("prime %d\n", prime);
  
  int right = -1;
  while (read(left, &curr, sizeof(int))) {
    if (curr % prime == 0)
      continue;
    if (right == -1) {
      int pipes[2]; pipe(pipes);
      if (!fork()) {
        close(left);
        close(pipes[WRITE]);
        drops(pipes[READ]);
        exit(0);
      } else {
        close(pipes[READ]);
        right = pipes[WRITE];
      }
    }
    // right process have been created
    write(right, &curr, sizeof(int));
  }
  // now that can't read from left. Close everything.
  close(right);
  wait(0); // wait for child to complete
  close(left);
  return 0;
}

int main(int argc, char *argv[]) {
  int pipes[2]; pipe(pipes);
  if (!fork()) {
    close(pipes[WRITE]);
    drops(pipes[READ]);
    exit(0);
  }
  for (int i = 2; i <= MAX_SIZE; i++) {
    write(pipes[WRITE], &i, sizeof(int));
  }
  close(pipes[WRITE]);
  wait(0);
  exit(0);
}
