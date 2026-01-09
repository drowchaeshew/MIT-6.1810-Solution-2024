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


// void *mmap(void *, uint64, int, int, int, int);
uint64
sys_mmap(void)
{
  int sz;
  int prot;
  int flag;

  struct file *f; // Maybee of use

  // The 0th argument is always 0. No need to get it.
  argint(1, &sz);
  argint(2, &prot); // PROT_READ, PROT_WRITE or both
  argint(3, &flag); // MAP_PRIVATE, or MAP_SHARED. (No need to share mem on MAP_SHARED)
  argfd(4, 0, &f);
  // The 5th argument is always 0. No need to get it. (File offset. Maybe of use later)

  // Remember to ROUND the sz.
  // NOTE: don't write back the padding part to file.

  // 4. Find out how user proc mem is freed, and insert code
  //    to free PTE_M pages. (Mostly write back to file when MAP_SHARED)
  // 5. How to manage multiple mmaped files? 

  // Some extra hints:
  // 1. The file mappped is opened. With fd, we can get the file 
  //    and the inode, so it's easy to write to the file. (TODO How to get it?)
  // 2. Ummap on exit()
  // 3. Copoy on fork()

  // Check prot here: prot & (f->readable & f->writeable)
  if (
    (f->readable != !!(prot & PROT_READ)) 
    || (f->writable != !!(prot && PROT_WRITE))) {
    return -1;
  }

  struct proc *proc = myproc();
  struct vma *vp; // , *empty;

  // TODO abstract as valloc()
  // empty = 0;
  // for (vp = &proc->vma[0]; vp < &proc->vma[NOVMA]; vp++) {
  //   // TODO now we assume that everything should be the same.
  //   // Stretagy can be changed later.
  //   if (vp->fd == fd && vp->prot == prot && vp->flag == flag && vp->sz == sz) {
  //     break;
  //   }
  //   if (!vp->valid)
  //     empty = vp;
  // }

  // if (vp == &proc->vma[NOVMA]) {
  //   if (empty == 0)
  //     panic("vget: No free vma");
  //   vp = empty;
  // }

  // TODO really simple achieve: Just use the first slot.
  vp = &proc->vma[0];

  vp->file = f;
  vp->prot = prot;
  vp->flag = flag;
  vp->sz = sz;

  filedup(vp->file);

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
  // MMAPBASE + NOFILE * 0x80000 = 0x09800000, 
  // still far away from VMMAX.
  // Good solution. Mipa~

  // TODO LATER 
  // mem beyond sz is not guard. 
  // if (sz % PGSIZE) != 0, 
  // then for addr satisfying sz ~ PGROUNDUP(sz),
  // read / write such address won't trigger a page fault.
  // Take care.
  sz = PGROUNDUP(sz);

  // A proper position: 0x90000000 (arbitary)
  vp->uva = MMAPBASE; // TODO adujst this later.

  vp->valid = 1;
  return vp->uva;
}

struct vma *vget(uint va);
void vstore(uint64 va, int write_back);

uint64
sys_munmap(void)
{
  uint64 addr;
  int sz; 

  argaddr(0, &addr);
  argint(1, &sz);

  // TODO Later 
  // Assume that it will always unmap the whole vma.

  struct vma *vp = vget(addr);
  if (vp == 0) {
    return -1;
  }
  vp->valid = 0;

  vstore(vp->uva, !!(vp->flag & MAP_SHARED));
  fileclose(vp->file);

  return 0;
}


// Given a va (user), 
// return the vma related to it.
// return 0 for not found.
struct vma *
vget(uint va)
{
  // Let's find the va first.
  // For each valid vma as vp, check if va is within [vp->uva ~ vp->uva + vp->sz)

  // TODO LATER 
  // as a demo, there can be only one vma. 
  struct vma *vp; 

  struct proc *proc = myproc();
  if ((vp = &proc->vma[0])->valid == 0) {
    return 0;
  }
  return vp;
}


// TODO bad naming.
// With a given va, 
// Load related file content into pagetable
int 
vload(uint64 va)
{
  struct vma *vp = vget(va);
  struct proc *proc = myproc();


  char *mem = kalloc();
  if (mem == 0) {
    return 0;
  }

  memset(mem, 0, PGSIZE);
  // Write something to the page. 
  // !! Always rember to free mem on failure !!

  // Can't use fileread(proc->ofile[vp->fd], ...): 
  // This will change the file->offset.

  // See struct vma.
  va = PGROUNDDOWN(va);
  int offset = va - vp->uva;

  struct inode *ip = vp->file->ip;

  ilock(ip);
  if (readi(ip, 0, (uint64)mem, offset, PGSIZE) == 0) {
    iunlock(ip);
    kfree(mem);
    return -1;
  }
  iunlock(ip);

  // No need to consider PTE_X.
  int perm = PTE_U;
  if (vp->prot & PROT_WRITE) perm |= PTE_W;
  if (vp->prot & PROT_READ)  perm |= PTE_R;

  if (mappages(proc->pagetable, va, PGSIZE, (uint64) mem, perm) != 0) {
    kfree(mem);
    return -1;
  }

  return 0;
}


// Unmap the rleated va to 
// Write the given page to the file related to vma.
void
vstore(uint64 va, int write_back)
{
  struct proc *proc = myproc();
  // if (mappages(proc->pagetable, va, PGSIZE, (uint64) mem, perm) != 0) {

  // TODO handle failure
  // We now assume that va is always at the begining of a vma.
  // We now assume that va is always the first vma
  int size = proc->vma[0].sz;

  uvmunmap(proc->pagetable, va, PGROUNDUP(size) / PGSIZE, 1);

  // TODO LATER not written back now.
}