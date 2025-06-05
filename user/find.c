#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"

const char *filename(const char *path) {
  // TODO 
  // not handled: path ends with a slash
  const char *p;
  for(p=path+strlen(path); p >= path && *p != '/'; p--); 
  return p + 1;
}

void find(const char *path, const char *name) {

  // 1. Read all entries in `path`;
  // 2. switch file type. 
  //    case org-files, OK. So judge it;
  // 3. if dir, invoke find(dir_path, name);
  // 4. If matches, print files' path. 

  // Step 1.
  int fd; 
  struct stat st;
  char buf[512], *p;
  struct dirent de;

  if ((fd = open(path, O_RDONLY)) < 0) {
    fprintf(2, "find: cannot open %s\n", path);
    return;
  }
  
  if (fstat(fd, &st) < 0) {
    fprintf(2, "find: cannot stat %s\n", path);
    close(fd);
    return;
  }

  switch(st.type) {
  case T_DEVICE: 
  case T_FILE:
    if (strcmp(filename(path), name) == 0) {
      printf("%s\n", path);
    } 
    break;
  case T_DIR: 
    strcpy(buf, path);
    p = buf + strlen(buf); 
    *p++ = '/';
    while (read(fd, &de, sizeof(de))) {
      if (de.inum == 0) 
        continue;
      if (strcmp(de.name, ".") == 0 || strcmp(de.name, "..") == 0)
        continue;
      memmove(p, de.name, DIRSIZ);
      p[DIRSIZ] = 0;
      if(stat(buf, &st) < 0){
        printf("ls: cannot stat %s\n", buf);
        continue;
      }
      find(buf, name);
    }
  }
  close(fd);
}



int
main(int argc, char *argv[])
{
  if(argc < 3){
    fprintf(2, "usage: ls directory pattern\n");
    exit(0);
  }
  find(argv[1], argv[2]);
  exit(0);
}
