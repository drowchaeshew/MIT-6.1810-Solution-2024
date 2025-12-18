// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

// 思路是将大页和小页，用不同的链表处理。
// 不合并连续的小页，即使它们能被拼成大页；
// 如果大页链表内容不足，则分配小页。

// 计划从三个阶段进行：
// 1. 物理页面管理：在 kalloc.c 中； 
// 2. 内核虚拟内存管理：在 vm.c 中；
// 3. 用户虚拟内存管理：也在 vm.c 中。

struct {
  struct spinlock lock;
  struct run *freelist;
  struct run *super_freelist;
} kmem;

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  // 这个函数只在 kinit 中被用到了
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  // 根据是否是 Super Page 对齐判断是要释放小页还是大页。
  for(; p + PGSIZE <= (char*)pa_end; ) {
    if (((uint64)p % SUPERPGSIZE) == 0 && p + SUPERPGSIZE <= (char *)pa_end) {
      kfree_super(p);
      p += SUPERPGSIZE;
    } else {
      kfree(p);
      p += PGSIZE;
    }
  }
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

void 
kfree_super(void *pa)
{
  struct run *r;

  if(((uint64)pa % SUPERPGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, SUPERPGSIZE);

  r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.super_freelist;
  kmem.super_freelist = r;
  release(&kmem.lock);
}

void kborrow(void);
// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if (!r) {
    release(&kmem.lock);
    kborrow();
    acquire(&kmem.lock);
    r = kmem.freelist; 
  }
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}

void *
kalloc_super(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.super_freelist;
  if(r)
    kmem.super_freelist = r->next;
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, SUPERPGSIZE);
  return (void*)r;
}

void
kborrow(void)
{
  char *r = kalloc_super();
  if (!r)
    return;
  for (char *p = r; p < r + SUPERPGSIZE; p += PGSIZE) {
    kfree(p);
  }
}