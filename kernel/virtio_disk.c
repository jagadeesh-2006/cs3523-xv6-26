//
// driver for qemu's virtio disk device.
// uses qemu's mmio interface to virtio.
//
// qemu ... -drive file=fs.img,if=none,format=raw,id=x0 -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "virtio.h"
#include "proc.h"

// the address of virtio mmio register r.
#define R(r) ((volatile uint32 *)(VIRTIO0 + (r)))
int disk_fail_sim       = -1;
int current_raid_level  = 0;
int current_sched_policy = FCFS;
int disk_head_pos       = 0;
struct disk_req disk_queue[MAX_DISK_REQ];
int queue_size          = 0;
static struct disk {
  // a set (not a ring) of DMA descriptors, with which the
  // driver tells the device where to read and write individual
  // disk operations. there are NUM descriptors.
  // most commands consist of a "chain" (a linked list) of a couple of
  // these descriptors.
  struct virtq_desc *desc;

  // a ring in which the driver writes descriptor numbers
  // that the driver would like the device to process.  it only
  // includes the head descriptor of each chain. the ring has
  // NUM elements.
  struct virtq_avail *avail;

  // a ring in which the device writes descriptor numbers that
  // the device has finished processing (just the head of each chain).
  // there are NUM used ring entries.
  struct virtq_used *used;

  // our own book-keeping.
  char free[NUM];  // is a descriptor free?
  uint16 used_idx; // we've looked this far in used[2..NUM].

  // track info about in-flight operations,
  // for use when completion interrupt arrives.
  // indexed by first descriptor index of chain.
  struct {
    struct buf *b;
    char status;
  } info[NUM];

  // disk command headers.
  // one-for-one with descriptors, for convenience.
  struct virtio_blk_req ops[NUM];
  
  struct spinlock vdisk_lock;
  
} disk;

void
virtio_disk_init(void)
{
  uint32 status = 0;

  initlock(&disk.vdisk_lock, "virtio_disk");

  if(*R(VIRTIO_MMIO_MAGIC_VALUE) != 0x74726976 ||
     *R(VIRTIO_MMIO_VERSION) != 2 ||
     *R(VIRTIO_MMIO_DEVICE_ID) != 2 ||
     *R(VIRTIO_MMIO_VENDOR_ID) != 0x554d4551){
    panic("could not find virtio disk");
  }
  
  // reset device
  *R(VIRTIO_MMIO_STATUS) = status;

  // set ACKNOWLEDGE status bit
  status |= VIRTIO_CONFIG_S_ACKNOWLEDGE;
  *R(VIRTIO_MMIO_STATUS) = status;

  // set DRIVER status bit
  status |= VIRTIO_CONFIG_S_DRIVER;
  *R(VIRTIO_MMIO_STATUS) = status;

  // negotiate features
  uint64 features = *R(VIRTIO_MMIO_DEVICE_FEATURES);
  features &= ~(1 << VIRTIO_BLK_F_RO);
  features &= ~(1 << VIRTIO_BLK_F_SCSI);
  features &= ~(1 << VIRTIO_BLK_F_CONFIG_WCE);
  features &= ~(1 << VIRTIO_BLK_F_MQ);
  features &= ~(1 << VIRTIO_F_ANY_LAYOUT);
  features &= ~(1 << VIRTIO_RING_F_EVENT_IDX);
  features &= ~(1 << VIRTIO_RING_F_INDIRECT_DESC);
  *R(VIRTIO_MMIO_DRIVER_FEATURES) = features;

  // tell device that feature negotiation is complete.
  status |= VIRTIO_CONFIG_S_FEATURES_OK;
  *R(VIRTIO_MMIO_STATUS) = status;

  // re-read status to ensure FEATURES_OK is set.
  status = *R(VIRTIO_MMIO_STATUS);
  if(!(status & VIRTIO_CONFIG_S_FEATURES_OK))
    panic("virtio disk FEATURES_OK unset");

  // initialize queue 0.
  *R(VIRTIO_MMIO_QUEUE_SEL) = 0;

  // ensure queue 0 is not in use.
  if(*R(VIRTIO_MMIO_QUEUE_READY))
    panic("virtio disk should not be ready");

  // check maximum queue size.
  uint32 max = *R(VIRTIO_MMIO_QUEUE_NUM_MAX);
  if(max == 0)
    panic("virtio disk has no queue 0");
  if(max < NUM)
    panic("virtio disk max queue too short");

  // allocate and zero queue memory.
  disk.desc = kalloc();
  disk.avail = kalloc();
  disk.used = kalloc();
  if(!disk.desc || !disk.avail || !disk.used)
    panic("virtio disk kalloc");
  memset(disk.desc, 0, PGSIZE);
  memset(disk.avail, 0, PGSIZE);
  memset(disk.used, 0, PGSIZE);

  // set queue size.
  *R(VIRTIO_MMIO_QUEUE_NUM) = NUM;

  // write physical addresses.
  *R(VIRTIO_MMIO_QUEUE_DESC_LOW) = (uint64)disk.desc;
  *R(VIRTIO_MMIO_QUEUE_DESC_HIGH) = (uint64)disk.desc >> 32;
  *R(VIRTIO_MMIO_DRIVER_DESC_LOW) = (uint64)disk.avail;
  *R(VIRTIO_MMIO_DRIVER_DESC_HIGH) = (uint64)disk.avail >> 32;
  *R(VIRTIO_MMIO_DEVICE_DESC_LOW) = (uint64)disk.used;
  *R(VIRTIO_MMIO_DEVICE_DESC_HIGH) = (uint64)disk.used >> 32;

  // queue is ready.
  *R(VIRTIO_MMIO_QUEUE_READY) = 0x1;

  // all NUM descriptors start out unused.
  for(int i = 0; i < NUM; i++)
    disk.free[i] = 1;

  // tell device we're completely ready.
  status |= VIRTIO_CONFIG_S_DRIVER_OK;
  *R(VIRTIO_MMIO_STATUS) = status;

  // plic.c and trap.c arrange for interrupts from VIRTIO0_IRQ.
}

// find a free descriptor, mark it non-free, return its index.
static int
alloc_desc()
{
  for(int i = 0; i < NUM; i++){
    if(disk.free[i]){
      disk.free[i] = 0;
      return i;
    }
  }
  return -1;
}

// mark a descriptor as free.
static void
free_desc(int i)
{
  if(i >= NUM)
    panic("free_desc 1");
  if(disk.free[i])
    panic("free_desc 2");
  disk.desc[i].addr = 0;
  disk.desc[i].len = 0;
  disk.desc[i].flags = 0;
  disk.desc[i].next = 0;
  disk.free[i] = 1;
  wakeup(&disk.free[0]);
}

// free a chain of descriptors.
static void
free_chain(int i)
{
  while(1){
    int flag = disk.desc[i].flags;
    int nxt = disk.desc[i].next;
    free_desc(i);
    if(flag & VRING_DESC_F_NEXT)
      i = nxt;
    else
      break;
  }
}

// allocate three descriptors (they need not be contiguous).
// disk transfers always use three descriptors.
static int
alloc3_desc(int *idx)
{
  for(int i = 0; i < 3; i++){
    idx[i] = alloc_desc();
    if(idx[i] < 0){
      for(int j = 0; j < i; j++)
        free_desc(idx[j]);
      return -1;
    }
  }
  return 0;
}

// void
// virtio_disk_rw(struct buf *b, int write)
// {
//   uint64 sector = b->blockno * (BSIZE / 512);

//   acquire(&disk.vdisk_lock);

//   // the spec's Section 5.2 says that legacy block operations use
//   // three descriptors: one for type/reserved/sector, one for the
//   // data, one for a 1-byte status result.

//   // allocate the three descriptors.
//   int idx[3];
//   while(1){
//     if(alloc3_desc(idx) == 0) {
//       break;
//     }
//     sleep(&disk.free[0], &disk.vdisk_lock);
//   }

//   // format the three descriptors.
//   // qemu's virtio-blk.c reads them.

//   struct virtio_blk_req *buf0 = &disk.ops[idx[0]];

//   if(write)
//     buf0->type = VIRTIO_BLK_T_OUT; // write the disk
//   else
//     buf0->type = VIRTIO_BLK_T_IN; // read the disk
//   buf0->reserved = 0;
//   buf0->sector = sector;

//   disk.desc[idx[0]].addr = (uint64) buf0;
//   disk.desc[idx[0]].len = sizeof(struct virtio_blk_req);
//   disk.desc[idx[0]].flags = VRING_DESC_F_NEXT;
//   disk.desc[idx[0]].next = idx[1];

//   disk.desc[idx[1]].addr = (uint64) b->data;
//   disk.desc[idx[1]].len = BSIZE;
//   if(write)
//     disk.desc[idx[1]].flags = 0; // device reads b->data
//   else
//     disk.desc[idx[1]].flags = VRING_DESC_F_WRITE; // device writes b->data
//   disk.desc[idx[1]].flags |= VRING_DESC_F_NEXT;
//   disk.desc[idx[1]].next = idx[2];

//   disk.info[idx[0]].status = 0xff; // device writes 0 on success
//   disk.desc[idx[2]].addr = (uint64) &disk.info[idx[0]].status;
//   disk.desc[idx[2]].len = 1;
//   disk.desc[idx[2]].flags = VRING_DESC_F_WRITE; // device writes the status
//   disk.desc[idx[2]].next = 0;

//   // record struct buf for virtio_disk_intr().
//   b->disk = 1;
//   disk.info[idx[0]].b = b;

//   // tell the device the first index in our chain of descriptors.
//   disk.avail->ring[disk.avail->idx % NUM] = idx[0];

//   __sync_synchronize();

//   // tell the device another avail ring entry is available.
//   disk.avail->idx += 1; // not % NUM ...

//   __sync_synchronize();

//   *R(VIRTIO_MMIO_QUEUE_NOTIFY) = 0; // value is queue number

//   // Wait for virtio_disk_intr() to say request has finished.
//   while(b->disk == 1) {
//     sleep(b, &disk.vdisk_lock);
//   }

//   disk.info[idx[0]].b = 0;
//   free_chain(idx[0]);

//   release(&disk.vdisk_lock);
// }

void
virtio_disk_rw(struct buf *b, int write)
{
  struct proc *p = myproc();
 
  acquire(&disk.vdisk_lock);
 
  // --- Enqueue ---
  if(queue_size >= MAX_DISK_REQ) panic("disk queue full");
  disk_queue[queue_size].b        = b;
  disk_queue[queue_size].block_no = (int)b->blockno;
  disk_queue[queue_size].priority = p ? p->qlevel : 0;
  disk_queue[queue_size].p        = p;
  queue_size++;
 
  // --- Schedule ---
  int target       = sched_pick_target();
  struct buf  *sb  = disk_queue[target].b;
  struct proc *owner = disk_queue[target].p;
 
  // Latency model: |current_head - requested_block| + C
  int dist    = disk_queue[target].block_no - disk_head_pos;
  if(dist < 0) dist = -dist;
  int latency = dist + ROTATIONAL_DELAY;
 
  // Update head position
  disk_head_pos = disk_queue[target].block_no;
 
  // Remove served entry from queue
  for(int i = target; i < queue_size - 1; i++)
    disk_queue[i] = disk_queue[i + 1];
  queue_size--;
 
  // Update per-process statistics 
  if(owner){
    owner->disk_reads         += !write;
    owner->disk_writes        += write;
    owner->total_disk_latency += latency;
  }
 
  release(&disk.vdisk_lock);
  // Lock is now free; virtio_disk_issue will re-acquire it.
 
  // Direct block→sector, no RAID
  uint64 sector = (uint64)sb->blockno * (BSIZE / 512);
  virtio_disk_issue(sb, write, sector);
}
void
virtio_disk_issue(struct buf *b, int write, uint64 sector)
{
  acquire(&disk.vdisk_lock);
 
  int idx[3];
  // allocate the three descriptors.
  while(1){
    if(alloc3_desc(idx) == 0) break;
    sleep(&disk.free[0], &disk.vdisk_lock);
  }
  // format the three descriptors.
  // qemu's virtio-blk.c reads them.
  struct virtio_blk_req *buf0 = &disk.ops[idx[0]];
  buf0->type     = write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
  buf0->reserved = 0;
  buf0->sector   = sector;
 
  disk.desc[idx[0]].addr  = (uint64)buf0;
  disk.desc[idx[0]].len   = sizeof(struct virtio_blk_req);
  disk.desc[idx[0]].flags = VRING_DESC_F_NEXT;
  disk.desc[idx[0]].next  = idx[1];
 
  disk.desc[idx[1]].addr  = (uint64)b->data;
  disk.desc[idx[1]].len   = BSIZE;
  disk.desc[idx[1]].flags = write ? 0 : VRING_DESC_F_WRITE;
  disk.desc[idx[1]].flags |= VRING_DESC_F_NEXT;
  disk.desc[idx[1]].next = idx[2];
 

  disk.info[idx[0]].status = 0xff; // device writes 0 on success
  disk.desc[idx[2]].addr = (uint64) &disk.info[idx[0]].status;
  disk.desc[idx[2]].len    = 1;
  disk.desc[idx[2]].flags  = VRING_DESC_F_WRITE;    // device writes the status
  disk.desc[idx[2]].next   = 0;
  
  // record struct buf for virtio_disk_intr().
  b->disk = 1;
  disk.info[idx[0]].b = b;
  // tell the device the first index in our chain of descriptors.
  disk.avail->ring[disk.avail->idx % NUM] = idx[0];
  __sync_synchronize();
  // tell the device another avail ring entry is available.
  disk.avail->idx += 1; // not % NUM ...
  __sync_synchronize();
  *R(VIRTIO_MMIO_QUEUE_NOTIFY) = 0; // value is queue number
  // Wait for virtio_disk_intr() to say request has finished.
  while(b->disk == 1) {
    sleep(b, &disk.vdisk_lock);
  }
  disk.info[idx[0]].b = 0;
  free_chain(idx[0]);
 
  release(&disk.vdisk_lock);
}
// Called with disk.vdisk_lock held.  Returns the queue index of the request
// that should be served next according to the current policy.
int
sched_pick_target(void)
{
  int target = 0; // FCFS: always serve the oldest request (index 0)
 
  if(current_sched_policy == SSTF){
    int min_dist = 0x7FFFFFFF;
    for(int i = 0; i < queue_size; i++){
      int d = disk_queue[i].block_no - disk_head_pos;
      if(d < 0) d = -d;
      // Tie-break: prefer higher-priority process (PA2 integration)
      if(d < min_dist ||
         (d == min_dist && disk_queue[i].priority > disk_queue[target].priority)){
        min_dist = d;
        target   = i;
      }
    }
  }
  return target;
}
void
virtio_disk_intr()
{
  acquire(&disk.vdisk_lock);

  // the device won't raise another interrupt until we tell it
  // we've seen this interrupt, which the following line does.
  // this may race with the device writing new entries to
  // the "used" ring, in which case we may process the new
  // completion entries in this interrupt, and have nothing to do
  // in the next interrupt, which is harmless.
  *R(VIRTIO_MMIO_INTERRUPT_ACK) = *R(VIRTIO_MMIO_INTERRUPT_STATUS) & 0x3;

  __sync_synchronize();

  // the device increments disk.used->idx when it
  // adds an entry to the used ring.

  while(disk.used_idx != disk.used->idx){
    __sync_synchronize();
    int id = disk.used->ring[disk.used_idx % NUM].id;

    if(disk.info[id].status != 0)
      panic("virtio_disk_intr status");

    struct buf *b = disk.info[id].b;
    b->disk = 0;   // disk is done with buf
    wakeup(b);

    disk.used_idx += 1;
  }

  release(&disk.vdisk_lock);
}
void
virtio_disk_rw_swap(struct buf *b, int write)
{
  struct proc *p = myproc();
 
  acquire(&disk.vdisk_lock);
 
  if(queue_size >= MAX_DISK_REQ) panic("disk queue full");
  disk_queue[queue_size].b        = b;
  disk_queue[queue_size].block_no = (int)b->blockno;
  disk_queue[queue_size].priority = p ? p->qlevel : 0;
  disk_queue[queue_size].p        = p;
  queue_size++;
 
  int target       = sched_pick_target();
  struct buf  *sb  = disk_queue[target].b;
  struct proc *owner = disk_queue[target].p;
 
  int dist    = disk_queue[target].block_no - disk_head_pos;
  if(dist < 0) dist = -dist;
  int latency = dist + ROTATIONAL_DELAY;
 
  disk_head_pos = disk_queue[target].block_no;
 
  for(int i = target; i < queue_size - 1; i++)
    disk_queue[i] = disk_queue[i + 1];
  queue_size--;
 
  if(owner){
    owner->disk_reads         += !write;
    owner->disk_writes        += write;
    owner->total_disk_latency += latency;
  }
 
  // Release before raid_logic — raid_logic will call virtio_disk_issue
  // (which re-acquires) and possibly bread (which calls virtio_disk_rw,
  // which also re-acquires).  Holding the lock across either would deadlock.
  release(&disk.vdisk_lock);
 
  raid_logic(sb, write);
}
// raid_logic — maps a logical swap buf to physical I/O according to the
//              current RAID level.
void
raid_logic(struct buf *b, int write)
{
  uint64 bno = b->blockno;
  int    N   = NUM_DISKS;   // 4
 
  if(current_raid_level == 0){
    // -----------------------------------------------------------------------
    // RAID 0 — Striping
    //   disk   = bno % N        (which simulated disk)
    //   sector = (bno / N) * (BSIZE/512)   (block within that disk)
    // -----------------------------------------------------------------------
    uint64 sector = (bno / (uint64)N) * (BSIZE / 512);
    virtio_disk_issue(b, write, sector);
  }
  else if(current_raid_level == 1){
    // -----------------------------------------------------------------------
    // RAID 1 — Mirroring
    //   Write: send to 2 copies (both at the same sector, simulated).
    //   Read:  serve from the primary copy.
    // -----------------------------------------------------------------------
    uint64 sector = bno * (BSIZE / 512);
    if(write){
      virtio_disk_issue(b, 1, sector); // primary
      virtio_disk_issue(b, 1, sector); // mirror
    } else {
      virtio_disk_issue(b, 0, sector);
    }
  }
  else if(current_raid_level == 5){
    // -----------------------------------------------------------------------
    // RAID 5 — Striping with distributed parity
    //
    //   parity_disk = bno % N
    //   stripe      = bno / (N-1)          (which stripe row)
    //   sector      = stripe * (BSIZE/512)  (sector address within a disk)
    //
    //   Write: issue data write, then read-modify-write the parity block.
    //   Read:  direct read, or reconstruct from XOR if disk is failed.
    // -----------------------------------------------------------------------
    int    p_disk = (int)(bno % (uint64)N);
    uint64 stripe = bno / (uint64)(N - 1);
    uint64 sector = stripe * (BSIZE / 512);
 
    if(write){
      // 1. Write the new data block.
      virtio_disk_issue(b, 1, sector);
 
      // 2. Compute parity = XOR of all data blocks in the stripe.
      //    We use a dedicated parity buf from a swap-reserved block range.
      //    For simulation we allocate block number at (p_disk * SWAPBLOCKS + stripe)
      //    but since we only have one disk in the QEMU setup we just write
      //    parity to a separate sector offset.
      struct buf *pbuf = bread(ROOTDEV, (uint)(p_disk * 1000 + (uint)stripe));
      // Reset then XOR with the new data.
      memset(pbuf->data, 0, BSIZE);
      xor_pages(pbuf, &b, 1);
      // Write parity.
      virtio_disk_issue(pbuf, 1, sector);
      brelse(pbuf);
    }
    else {
      // Read path.
      if(p_disk == disk_fail_sim){
        // Reconstruct: XOR all other N-1 data/parity blocks.
        memset(b->data, 0, BSIZE);
        for(int i = 0; i < N; i++){
          if(i == p_disk) continue;
          struct buf *tmp = bread(ROOTDEV, (uint)(i * 1000 + (uint)stripe));
          xor_pages(b, &tmp, 1);
          brelse(tmp);
        }
      } else {
        virtio_disk_issue(b, 0, sector);
      }
    }
  }
  // Unknown RAID level: fall through silently (or add a panic if desired).
}
// xor all the blocks in the stripe to compute parity (for RAID 5 writes)
;// xor_pages — XOR num_data source bufs into parity_buf.
//             parity_buf->data is zeroed first, then each source is XOR-ed in.
// ---------------------------------------------------------------------------
void
xor_pages(struct buf *parity_buf, struct buf *data_bufs[], int num_data)
{
  memset(parity_buf->data, 0, BSIZE);
  for(int i = 0; i < num_data; i++){
    for(int j = 0; j < BSIZE; j++){
      parity_buf->data[j] ^= data_bufs[i]->data[j];
    }
  }
}
 