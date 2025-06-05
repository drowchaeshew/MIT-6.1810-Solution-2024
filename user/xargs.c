#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fcntl.h"
#include "kernel/param.h"

#define BUFSIZ 256

int readline(char *p) {
  int ret = 0;
  while (read(0, p, 1)) {
    if (*p == '\n') {
      *p = '\0';
      return ret;
    }
    ret += 1;
    p++;
  }
  return ret; 
}

int main(int argc, char *argv[]) {
  char elems[BUFSIZ]; // NOT TO BIG THOUGH
  int pid;

  char *new_argv[MAXARG];
  char *p = elems;
  for (int i = 1; i < argc; i++) {
    new_argv[i - 1] = p;
    strcpy(p, argv[i]);
    p += strlen(argv[i]);
    p++;
  }

  new_argv[argc - 1] = p;

  while (1) {
    p = new_argv[argc - 1];
    if (!readline(p)) {
      break;
    }
    // for (int i = 0; i < argc; i++) {
    //   printf("%s\n", new_argv[i]);
    // }
    if (!(pid = fork())) {
      exec(new_argv[0], new_argv);
      fprintf(2, "exec failed\n");
      exit(1);
    } else {
      wait(&pid);
    }
  }
  exit(0);
}

// int
// main(int argc, char *argv[])
// {
//   char new_argv[MAXARG][BUFSIZ];
// 
//   if (argc < 2) {
//     fprintf(2, "usage: xargs command");
//     exit(0);
//   }
// 
//   int i; 
//   for (i = 0; i < argc; i++) {
//     strcpy(new_argv[i], argv[i]);
//   }
//   // i now equals to the number of original argvs 
// 
//   while (1) {
//     char *p = new_argv[i];
//     if (!readline(p))
//       break;
//     
//     for (i = 0; i <= argc; i++) {
//       if (!fork()) {
//         exec(new_argv[0], (char **)new_argv);
//       }
//     }
//   }
// 
//   exit(0);
// }
// # 
// 
