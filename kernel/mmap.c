// TODO check the necessity of these headers.

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"
#include "memlayout.h"

#define min(a, b) ((a) <= (b) ? (a): (b))
#define max(a, b) ((a) >= (b) ? (a): (b))


// Find a free vma and return it.
static struct vma *
valloc(int sz)
{
  struct vma *vp;
  struct proc *proc = myproc();

  // Since no other cpu will be handling this process,
  // and no kerneltrap will be manipulating vma, 
  // The lock is not required.
  // (Really?)
  for (vp = &proc->vma[0]; vp < &proc->vma[NOVMA]; vp++) {
    if (!vp->valid) {
      vp->valid = 1;
      break;
    }
  }

  if (vp == &proc->vma[NOVMA]) {
    return 0; // no free vma slot
  }

  // TODO! Where should the page place? 
  // LATER let's place the mapped page on somewhere, 
  // and now just consider on mapping one file.

  // As for where to map the file content, 
  // Let's do some calculation: 
  // 1. Each mmaped file can take at most 67 pages:
  //    ans = MAXFILE * BSIZE / PGSIZE
  // 2. Only 16 files can be opened:
  //    ans = NOFILE
  //
  // We can map the nth file (in proc->ofile) to 
  // MMAPBASE + n * 0x80000
  // 
  // Explanation: 0x80000 = 128 * PGSIZE > 67 * PGSIZE
  // MMAPBASE + NOFILE * 0x80000 = 0x90800000, 
  // still far away from VMMAX.
  // Good solution. Mipa~

  // TODO LATER 
  // mem beyond sz is not guard. 
  // if (sz % PGSIZE) != 0, 
  // then for addr satisfying sz ~ PGROUNDUP(sz),
  // read / write such address won't trigger a page fault.
  // Take care.
  // sz = PGROUNDUP(sz);

  // A proper position: 0x90000000 (arbitary)
  vp->start = MMAP(vp - &proc->vma[0]);
  vp->end = vp->start + sz;

  // TDOO enable this when debugging multiple mmap
  // printf("VMA alloced [%ld], vp->start = %lx\n", vp - &proc->vma[0], vp->start);

  return vp;
}

static struct vma *
vfind(uint64 va)
{
  struct vma *vp;
  struct vma *arr = myproc()->vma;

  for (vp = arr; vp < &arr[NOVMA]; vp++) {
    if (!vp->valid)
      continue;
    if (va >= vp->start && va < vp->end) {
      // printf("vfind(%lx) [%ld]\n", va, vp - arr);
      return vp;
    }
  }
  return 0;
}

// return written sz
static int 
vdump(uint64 va, struct inode *ip, int off, int sz)
{
  int ret;
  begin_op();
  ilock(ip); // Lock here: ensure ip->size won't change
  if (off > ip->size) {
    ret = 0;
    goto end;
  }
  sz = min(sz, ip->size - off);
  if (writei(ip, 1, va, off, sz) < 0) {
    ret = -1;
    goto end;
  }
end:
  iunlock(ip);
  end_op();
  return ret;
}

// void *mmap(void *, uint64, int, int, int, int);
uint64
sys_mmap(void)
{
  int sz;
  int prot;
  int flag;
  struct file *file;

  // The 0th argument is always 0. No need to get it.
  argint(1, &sz);
  argint(2, &prot); // PROT_READ, PROT_WRITE or both
  argint(3, &flag); // MAP_PRIVATE, or MAP_SHARED. (No need to share mem on MAP_SHARED)
  argfd(4, 0, &file);
  // The 5th argument is always 0. No need to get it. (File offset. Maybe of use later)

  // 4. Find out how user proc mem is freed, and insert code
  //    to free PTE_M pages. (Mostly write back to file when MAP_SHARED)
  // 5. How to manage multiple mmaped files? 

  // Some extra hints:
  // 1. The file mappped is opened. With fd, we can get the file 
  //    and the inode, so it's easy to write to the file. (TODO How to get it?)
  // 2. Ummap on exit()
  // 3. Copoy on fork()

  if ((prot & PROT_READ && !file->readable)
    || ((prot & PROT_WRITE) && (flag & MAP_SHARED) && !(file->writable))
  ) return -1;

  struct vma *vp;

  if ((vp = valloc(sz)) == 0)
    panic("vget: No free vma");

  filedup(file);
  vp->file = file;
  vp->prot = prot;
  vp->flag = flag;

  // NO lazy mode now.
  vload(vp->start);

  return vp->start;
}

uint64
sys_munmap(void)
{
  // Assumption:
  // * vp->start & vp->end are PGSIZE aligned
  uint64 start, end;
  int sz; 

  argaddr(0, &start);
  argint(1, &sz);

  struct vma *vp = vfind(start); 
  if (!vp) {
    return -1;
  }

  end = min(start + sz, vp->end);

  // TODO change start and end for PGSIZE alignement

  if (start == vp->start && end == vp->end) {
    printf("munmap: case-1\n");
  } else if (start == vp->start) {
    printf("munmap: case-2\n");
  } else {
    if (end < vp->end) {
      panic("You said no hole!");
    }
    printf("munmap: case-3\n");
  }

  pagetable_t pgtbl = myproc()->pagetable;
  struct inode *ip = vp->file->ip;


  // Now we assume the pages are all loaded.
  // For lazy mapped pages, consider them later.

  // TODO now we just unmap the whole vma.
  // 
  for (uint64 addr = start; addr < end; addr += PGSIZE) {
    if (vp->flag & MAP_SHARED) {
      // What if failed? (res < 0)? We don't care about it.
      vdump(addr, ip, addr - vp->start, PGSIZE);
    }
    uvmunmap(pgtbl, addr, 1, 1);
  }

  if (vp->start == start && vp->end == end) {
    fileclose(vp->file);
    vp->valid = 0;
  }
  return 0;
}

// TODO bad naming.
// With a given va, 
// Load related file content into pagetable
int 
vload(uint64 va)
{
  struct proc *proc = myproc();

  // TODO Later Multiple VMA 
  // TODO not alloc one, but also the wanted one!
  // NOTE: use va
  struct vma *vp = &proc->vma[0]; 
  struct inode *ip = vp->file->ip;


  // No need to consider PTE_X.
  int perm = PTE_U;
  if (vp->prot & PROT_WRITE) perm |= PTE_W;
  if (vp->prot & PROT_READ)  perm |= PTE_R;

  // Assumption:
  // vp->start is PGSIZE aligned


  // Write something to the page. 
  // !! Always rember to free mem on failure !!

  char *mem;
  for (uint64 addr = vp->start; addr < vp->end; addr += PGSIZE) {
    if ((mem = kalloc()) == 0) {
      // TODO handle the failure
      // uvmunmap()
    }
    int offset = addr - vp->start;

    memset(mem, 0, PGSIZE);
    ilock(ip);
    if (readi(ip, 0, (uint64)mem, offset, PGSIZE) < 0) {
      iunlock(ip);
      kfree(mem); // TODO unmmap former pages
      return -1;
    }
    iunlock(ip);

    if (mappages(proc->pagetable, addr, PGSIZE, (uint64) mem, perm) != 0) {
      kfree(mem); // TODO unmap former pages
      return -1;
    }
  }
  return 0;
}
