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

  if (prot & PROT_READ) {
    if (!f->readable) 
      return -1;
  }
  if (prot & PROT_WRITE) {
    if ((flag & MAP_SHARED) && !(f->writable))
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
  // sz = PGROUNDUP(sz);

  // A proper position: 0x90000000 (arbitary)
  vp->start = MMAPBASE; // TODO adujst this later.
  vp->end = vp->start + sz;

  vp->valid = 1;

  // NO lazy mode now.
  vload(vp->start);

  return vp->start;
}

uint64
sys_munmap(void)
{
  uint64 addr;
  int sz; 

  argaddr(0, &addr);
  argint(1, &sz);

  struct vma *vp = &myproc()->vma[0]; // TODO LATER Multiple VMA
  if (vp == 0) {
    return -1;
  }

  // TODO LATER
  // There's only three conditions: 
  // 1. vp->uva == addr, vp->sz == sz 
  //    # Remove the mapping
  // 2. vp->uva == addr, vp->sz > sz
  // 3. vp->uva < addr, addr + zs == vp->uva + vp->sz
  // Fail on other conditions.

  // addr = PGROUNDUP(addr);

  // TODO now only the whole vma. (addr == va->start, addr + sz == va->end)
  // The page is loaded lazily... So not all pages are written to the pagetable.

  pagetable_t pgtbl = myproc()->pagetable;

  // Not all pages are mapped! 
  // Check if the page is mapped first.
  // 
  // Assumption:
  // 1. vp->start & vp->end is PGSIZE aligned
  // 
  struct inode *ip = vp->file->ip;

  // TODO warning: `sz` is not used.
  // To determine where to end unmap

  // Now we assume the pages are all loaded once the first Page Fault occurs.
  // TODO now we just unmap the whole vma.
  // 
  for (uint64 addr = vp->start; addr < vp->end; addr += PGSIZE) {
    if (vp->flag & MAP_SHARED) {
      // Write this page to the file
      uint64 off = addr - vp->start;

      // TODO What if user is writing the file with fp at the same time?
      int sz = vp->file->ip->size;
      sz = sz > off ? sz - off : 0;
      sz = min(sz, PGSIZE);

      if (sz > 0) {
        begin_op();
        ilock(ip);
        if (writei(ip, 1, addr, off, sz) < 0) {
          // TOOD handler error
          iunlock(ip);
          end_op();
          return -1;
        }
        iunlock(ip);
        end_op();
      }
    }
    uvmunmap(pgtbl, addr, 1, 1);
  }

  // TODO LATER only do this when the whole vma is unmmapping.
  fileclose(vp->file);
  vp->valid = 0;

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
