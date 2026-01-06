// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define BUCKETS 13
#define BUF_HASH(dev, blockno) (((dev) + (blockno)) % BUCKETS)

struct {
  struct buf buf[NBUF];

  struct bucket {
    struct spinlock lock;
    struct buf *head;
  } bucket[BUCKETS + 1]; // The last bucket is for free caches
} bcache;

char bcache_name[BUCKETS][16];

void
binit(void)
{
  struct buf *b;

  for (int i = 0; i < BUCKETS + 1; ++i) {
    snprintf(bcache_name[i], 16, "bcache-%d", i);
    initlock(&bcache.bucket[i].lock, bcache_name[i]);
  }

  // Everyone in the free bucket.
  struct bucket *free = &bcache.bucket[BUCKETS];

  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    b->next = free->head;
    free->head = b;
    initsleeplock(&b->lock, "buffer");
  }
}


// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  int idx = BUF_HASH(dev, blockno);
  struct bucket *bucket = &bcache.bucket[idx];

  acquire(&bucket->lock);
  for (b = bucket->head; b; b = b->next) {
    if (b->dev == dev && b->blockno == blockno) {
        b->refcnt++;
        release(&bucket->lock);
        acquiresleep(&b->lock);
        return b;
    }
  }
  // Opps. Not cached.

  // We can't release bucket->lock now. 
  // If so, if another process is requesting the same block
  // at the same time, multiple buffer will be related to a 
  // same sector.
  // 
  // Then let's walk through the free bucket to get one.
  // Since we always acquire non-free bucket first, 
  // there should not be any dead-lock...hopefully...

  struct bucket *free = &bcache.bucket[BUCKETS];
  acquire(&free->lock);
  if (!free->head) {
    release(&free->lock);
    release(&bucket->lock);
    panic("bget: no buffers");
  }

  b = free->head;
  free->head = free->head->next;
  release(&free->lock);

  // b->refcnt == 0 is not checked. 
  // We assume bufs in free always satisfy it.
  b->next = bucket->head;
  bucket->head = b;

  b->dev = dev;
  b->blockno = blockno;
  b->valid = 0;
  b->refcnt = 1;
  release(&bucket->lock);
  acquiresleep(&b->lock);
  return b;
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  int idx = BUF_HASH(b->dev, b->blockno);
  struct bucket *bucket = &bcache.bucket[idx];

  acquire(&bucket->lock);
  b->refcnt--;
  if (b->refcnt == 0) {
    // 因为30个buf放到13个桶里，基本上不太会冲突——
    // 直接遍历够了
    if (bucket->head == b) {
      bucket->head = b->next;
    } else {
      int flag = 0;
      for (struct buf *bb = bucket->head; bb->next; bb = bb->next) {
        if (bb->next == b) {
          bb->next = bb->next->next;
          flag = 1;
          break;
        }
      }
      // Maybe not found...
      if (flag == 0) {
        // TODO release bucket->lock
        panic("brelse: Not found!\n");
      }
    }

    struct bucket *free = &bcache.bucket[BUCKETS];
    acquire(&free->lock);
    b->next = free->head;
    free->head = b;
    release(&free->lock);
  }
  release(&bucket->lock);
}

void
bpin(struct buf *b) {
  int idx = BUF_HASH(b->dev, b->blockno);
  struct bucket *bucket = &bcache.bucket[idx];

  acquire(&bucket->lock);
  b->refcnt++;
  release(&bucket->lock);
}

void
bunpin(struct buf *b) {
  int idx = BUF_HASH(b->dev, b->blockno);
  struct bucket *bucket = &bcache.bucket[idx];

  acquire(&bucket->lock);
  b->refcnt--;
  release(&bucket->lock);
}


