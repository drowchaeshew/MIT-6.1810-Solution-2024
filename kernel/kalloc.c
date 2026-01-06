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

struct kmem {
  struct spinlock lock;
  struct run *freelist;
  int size;
} kmems[NCPU];

char lock_names[NCPU][0x10];

void
kinit()
{
  for (int i = 0; i < NCPU; i++) {
    snprintf(lock_names[i], 0x10, "kmem-%d", i);
    initlock(&kmems[i].lock, lock_names[i]);
  }
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
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

  push_off();
  int id = cpuid(); 
  pop_off();

  struct kmem *kmem = &kmems[id];
  acquire(&kmem->lock);
  r->next = kmem->freelist;
  kmem->freelist = r;
  kmem->size += 1;
  release(&kmem->lock);
}


// #define STEAL_HALF

struct run *
steal(void);

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  push_off();
  int id = cpuid(); 
  pop_off();

  struct run *r;
  acquire(&kmems[id].lock);
  r = kmems[id].freelist;
  if(r) {
    kmems[id].freelist = r->next;
    kmems[id].size -= 1;
  }
  release(&kmems[id].lock);

  do {
    // If we got a page, no need for stealing.
    if (r) 
      break;

    // If no one's got free page, No need to insert pages.
    r = steal();
    if (!r) 
      break;
    
    struct run *lst = r->next; 

    struct kmem *kmem = &kmems[id];
    acquire(&kmem->lock);
    while (lst != 0) {
      struct run *node = lst;
      lst = lst->next;
      node->next = kmem->freelist;
      kmem->freelist = node;
      kmem->size += 1;
    }
    release(&kmem->lock);
  } while (0);

  if (r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}

struct run *
steal(void) {
  
  struct run *r = 0;

  while(!r) {
    int idx = 0, max_size = kmems[0].size;
    for (int i = 1; i < NCPU; ++i) {
      int size = kmems[i].size;
      if (size > max_size) {
        idx = i; 
        max_size = size;
      }
    }

    if (max_size == 0) {
      break; // now r == 0, indicate no free mem.
    }

    // now we need to steal mem from kmems[idx].
    struct kmem *kmem = &kmems[idx];

    acquire(&kmem->lock);
#ifndef STEAL_HALF
    // Let's make it easy: Just steal one page now.
    r = kmem->freelist; // This can be empty. 
    if (r) {
      kmem->freelist = r->next;
      r->next = 0;
    }
    kmem->size -= 1;
#else
    // Now let's try to steal half.
    struct run *quick = kmem->freelist;
    struct run *slow = quick;
    int n = 0;
    while (quick) {
      quick = quick->next;
      if (!quick) 
        break;
      quick = quick->next;
      slow = slow->next;
      n += 1;
    }

    r = kmem->freelist;
    // Still, r can be zero
    if (r) { 
      kmem->freelist = slow->next;
      slow->next = 0;
      kmem->size -= n;
    }
#endif
    release(&kmem->lock);

    // If we set r to a non-zero value, now it's time to break, 
    // and the stolen pages are returned to the caller.
    // Or else, try it again.
  }

  // If we got here, there are two conditions: 
  // 1. No free mem -> r == 0;
  // 2. Got a stolen list: r != 0;

  return r;
}