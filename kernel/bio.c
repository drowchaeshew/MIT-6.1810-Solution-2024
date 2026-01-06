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
#define FREE_BUCK BUCKETS
#define BUF_HASH(dev, blockno) (((dev) + (blockno)) % BUCKETS)

struct {
  struct buf buf[NBUF];

  struct bucket {
    struct spinlock lock;
    struct buf *free;
    struct buf *head;
  } bucket[BUCKETS + 1]; // The last bucket is for free bufs
} bcache;

char bcache_name[BUCKETS + 1][16];

void
binit(void)
{
  struct buf *b;

  for (int i = 0; i < BUCKETS + 1; ++i) {
    snprintf(bcache_name[i], 16, "bcache-%d", i);
    initlock(&bcache.bucket[i].lock, bcache_name[i]);
  }

  // Reserve a buf for each bucket. 
  // The left are saved in the free-bucket.
  struct bucket *free = &bcache.bucket[BUCKETS];

  for (int i = 0; i < NBUF; i++) {
    b = &bcache.buf[i];
    if (i < BUCKETS) {
      bcache.bucket[i].free = b;
    } else {
      b->next = free->head;
      free->head = b;
    }
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
  struct bucket *free = &bcache.bucket[FREE_BUCK];

  acquire(&bucket->lock);

  // The buf can be cached in three places. 
  // First, in the bucket->head list...
  for (b = bucket->head; b; b = b->next) {
    if (b->dev == dev && b->blockno == blockno)
      break;
  }

  // Second, the bucket->free
  if (!b) {
    b = bucket->free; 
    if (b && b->dev == dev && b->blockno == blockno) {
      bucket->free = 0;
      b->next = bucket->head;
      bucket->head = b;
    } else {
      b = 0; // set b = 0 to indicate the buf is still not found.
    } 
  }

  // Third, maybe in the free-bucket? 
  // Let's don't think about it.

  // IF buf found, return it.
  if (b) {
    b->refcnt++;
    release(&bucket->lock);
    acquiresleep(&b->lock);
    return b;
  }

  // Opps. Not cached.

  // Don't worry. Let's check the free buf in the same bucket first.
  b = bucket->free;
  if (b) {
    bucket->free = 0;
  }

  // Opps. No free page in current bucket. 
  // Let's try alloc one from free-bucket.
  if (!b) {

    acquire(&free->lock);
    if (free->head) {
      b = free->head;
      free->head = b->next;
    }
    release(&free->lock);
  }

  // We still can't release the bucket lock...
  // Reason: avoiding another process wanting the same buf
  // start running the function, finding that no buf such that
  // the buf moving code is run twice.
  // 
  // The lock should not be released until (or): 
  // 1. we've finished set a buf to the wanted bucket;
  // 2. No free buf can be moved.

  // Opps! Even the free-bucket don't have free pages! 
  // We have no choice but steal one from another bucket.free...
  if (!b) {
    for (int i = 0; i < BUCKETS; ++i) {
      struct bucket *another = &bcache.bucket[i];

      // This predication avoids dead-lock.
      // Dead lock may occur, if two processes with diff bucket are
      // both hungry for pages, and the free-bucket is empty.
      // If Bucket-A lock and require for Bucket-B's lock, and vice versa, 
      // dead lock happens.
      // But if we don't try to get lock of a bucket without free buf, 
      // we can avoid getting a hungry process, thus the deadlock is avoided.
      if (another->free) {
        acquire(&another->lock);
        // free buf can be used after the predication and before the acquirment of the lock.
        // So there's chance that another->free is 0 now.
        if (another->free) {
          b = another->free; 
          another->free = 0;
        }
        release(&another->lock);
      }
      if (b) {
        break;
      }
    }
    // This is the only way to let b == 0
  }

  // Sometimes going through the all bucket without a free buf 
  // doesn't mean there's no free buf. 
  // This is kind of arbitrary.
  if (!b) { // unlikely
    release(&bucket->lock);
    panic("bget: no buffers");
  }

  // Now add the buf to the bucket.
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
      if (flag == 0) { // Not likely
        release(&bucket->lock);
        panic("brelse: Not found!\n");
      }
    }

    if (!bucket->free) {
      bucket->free = b;
    } else {
      struct bucket *free = &bcache.bucket[FREE_BUCK];
      acquire(&free->lock);
      b->next = free->head;
      free->head = b;
      release(&free->lock);
    }
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


