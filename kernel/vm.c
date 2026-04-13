#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"

uint64 swap_in(struct proc *p, uint64 va);
int swap_out(struct proc *p, uint64 va, void *pa);
int evict_page(void);
void add_frame(struct proc *p, uint64 va, uint64 pa);
int ismapped(pagetable_t pagetable, uint64 va);
void set_refbit(uint64 pa);

struct frame
{
  int used;
  struct proc *p;
  uint64 pa;
  uint64 va;
  int refbit;
};

struct frame frame_table[MAX_FRAMES];
int clock_hand = 0;
struct spinlock frame_lock;
int stale_frame_cleanup_count = 0;

// swap space
char swap_space[MAX_SWAP][PGSIZE];
int swap_used[MAX_SWAP];
struct spinlock swap_lock;
/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[]; // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t)kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x4000000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext - KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP - (uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // allocate and map a kernel stack for each process.
  proc_mapstacks(kpgtbl);

  return kpgtbl;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if (mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// Initialize the kernel_pagetable, shared by all CPUs.
void kvminit(void)
{
  kernel_pagetable = kvmmake();

  initlock(&frame_lock, "frame");
  initlock(&swap_lock, "swap");

  for (int i = 0; i < MAX_FRAMES; i++)
  {
    frame_table[i].used = 0;
    frame_table[i].p = 0; // make sure this is also set
    frame_table[i].pa = 0;
    frame_table[i].va = 0;
    frame_table[i].refbit = 0;
  }

  for (int i = 0; i < MAX_SWAP; i++)
    swap_used[i] = 0;
}

// Switch the current CPU's h/w page table register to
// the kernel's page table, and enable paging.
void kvminithart()
{
  // wait for any previous writes to the page table memory to finish.
  sfence_vma();

  w_satp(MAKE_SATP(kernel_pagetable));

  // flush stale entries from the TLB.
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if (va >= MAXVA)
    panic("walk");

  for (int level = 2; level > 0; level--)
  {
    pte_t *pte = &pagetable[PX(level, va)];
    if (*pte & PTE_V)
    {
      pagetable = (pagetable_t)PTE2PA(*pte);
    }
    else
    {
      if (!alloc || (pagetable = (pde_t *)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if (va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if (pte == 0)
    return 0;
  if ((*pte & PTE_V) == 0)
    return 0;
  if ((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  set_refbit(pa);
  return pa;
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa.
// va and size MUST be page-aligned.
// Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  if ((va % PGSIZE) != 0)
    panic("mappages: va not aligned");

  if ((size % PGSIZE) != 0)
    panic("mappages: size not aligned");

  if (size == 0)
    panic("mappages: size");

  a = va;
  last = va + size - PGSIZE;
  for (;;)
  {
    if ((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if (*pte & PTE_V)
      panic("mappages: remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if (a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t)kalloc();
  if (pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. It's OK if the mappings don't exist.
// Optionally free the physical memory.
void uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if ((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for (a = va; a < va + npages * PGSIZE; a += PGSIZE)
  {
    if ((pte = walk(pagetable, a, 0)) == 0) // leaf page table entry allocated?
      continue;
    if ((*pte & PTE_V) == 0) // has physical page been allocated?
      continue;
    if (do_free)
    {
      uint64 pa = PTE2PA(*pte);
      kfree((void *)pa);
    }
    *pte = 0;
  }
}

// Allocate PTEs and physical memory to grow a process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
{
  char *mem;
  uint64 a;

  if (newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for (a = oldsz; a < newsz; a += PGSIZE)
  {
    mem = kalloc();
    if (mem == 0)
    {
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if (mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_R | PTE_U | xperm) != 0)
    {
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    // add_frame(myproc(), a, (uint64)mem);
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if (newsz >= oldsz)
    return oldsz;

  if (PGROUNDUP(newsz) < PGROUNDUP(oldsz))
  {
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for (int i = 0; i < 512; i++)
  {
    pte_t pte = pagetable[i];
    if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0)
    {
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    }
    else if (pte & PTE_V)
    {
      panic("freewalk: leaf");
    }
  }
  kfree((void *)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void uvmfree(pagetable_t pagetable, uint64 sz)
{
  if (sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz) / PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int uvmcopy(pagetable_t old, pagetable_t new, uint64 sz, struct proc *np)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for (i = 0; i < sz; i += PGSIZE)
  {
    if ((pte = walk(old, i, 0)) == 0)
      continue; // page table entry hasn't been allocated
    if ((*pte & PTE_V) == 0)
      continue; // physical page hasn't been allocated
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if ((mem = kalloc()) == 0)
      goto err;
    memmove(mem, (char *)pa, PGSIZE);
    if (mappages(new, i, PGSIZE, (uint64)mem, flags) != 0)
    {
      kfree(mem);
      goto err;
    }
    // add_frame(np, i, (uint64)mem); // track the new frame with refbit=1
  }
  return 0;

err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;

  pte = walk(pagetable, va, 0);
  if (pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;
  pte_t *pte;

  while (len > 0)
  {
    va0 = PGROUNDDOWN(dstva);
    if (va0 >= MAXVA)
      return -1;

    pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0)
    {
      if ((pa0 = vmfault(pagetable, va0, 0)) == 0)
      {
        return -1;
      }
    }

    pte = walk(pagetable, va0, 0);
    // forbid copyout over read-only user text pages.
    if ((*pte & PTE_W) == 0)
      return -1;

    n = PGSIZE - (dstva - va0);
    if (n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;

  while (len > 0)
  {
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0)
    {
      if ((pa0 = vmfault(pagetable, va0, 0)) == 0)
      {
        return -1;
      }
    }
    n = PGSIZE - (srcva - va0);
    if (n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while (got_null == 0 && max > 0)
  {
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if (n > max)
      n = max;

    char *p = (char *)(pa0 + (srcva - va0));
    while (n > 0)
    {
      if (*p == '\0')
      {
        *dst = '\0';
        got_null = 1;
        break;
      }
      else
      {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if (got_null)
  {
    return 0;
  }
  else
  {
    return -1;
  }
}

// allocate and map user memory if process is referencing a page
// that was lazily allocated in sys_sbrk().
// returns 0 if va is invalid or already mapped, or if
// out of physical memory, and physical address if successful.
uint64
vmfault(pagetable_t pagetable, uint64 va, int read)
{
  struct proc *p = myproc();

  if (p == 0)    // no process, can't handle page fault
    return 0;

  if (va >= p->sz)   // invalid access, va is outside process's memory
    return 0; 

  va = PGROUNDDOWN(va);

  if (ismapped(pagetable, va))   // already mapped, shouldn't be faulting on this
    return walkaddr(pagetable, va);

  p->vmstats.page_faults++;

  int vpn = va / PGSIZE;
  if (vpn >= MAX_PSYC_PAGES)   // invalid access, exceeds max process pages
    return 0;

  // swapped page
  if (p->swapped[vpn])
  {
    return swap_in(p, va);
  }

  // new page
  char *mem = kalloc();
  if (mem == 0)
    return 0;

  memset(mem, 0, PGSIZE);

  if (mappages(p->pagetable, va, PGSIZE,
               (uint64)mem, PTE_W | PTE_U | PTE_R) != 0)
  {
    kfree(mem);
    return 0;
  }

  add_frame(p, va, (uint64)mem);

  // p->vmstats.resident_pages++;

  return (uint64)mem;
}

int ismapped(pagetable_t pagetable, uint64 va) // returns 1 if va is mapped in pagetable, 0 otherwise
{
  pte_t *pte = walk(pagetable, va, 0);
  if (pte == 0)
  {
    return 0;
  }
  if (*pte & PTE_V)
  {
    return 1;
  }
  return 0;
}

void add_frame(struct proc *p, uint64 va, uint64 pa)   // add a frame to the frame table, evicting if necessary. loops until successful.
{
  // printf("add_frame: pid=%d va=0x%lx\n", p ? p->pid : -1, va);
  while (1)
  {
    acquire(&frame_lock);

    for (int i = 0; i < MAX_FRAMES; i++)
    {
      if (frame_table[i].used == 0)
      {
        frame_table[i].used = 1;
        frame_table[i].p = p;
        frame_table[i].va = va;
        frame_table[i].pa = pa;
        frame_table[i].refbit = 1;
        release(&frame_lock);
        
        p->vmstats.resident_pages++;   
        
        return;
      }
    }

    release(&frame_lock);

    // No free slot, try eviction
    if (!evict_page())
    {
      panic("no free frame slot");
    }
  }
}
struct frame *select_victim()  // select a victim frame to evict using the clock algorithm with MLFQ-aware preference
{
  // PASS 1: prefer low priority + refbit = 0
  for (int i = 0; i < MAX_FRAMES; i++)
  {
    struct frame *f = &frame_table[clock_hand];
    if (f->used && f->p == 0)
    {
      printf("PANIC: frame_table entry used but p==0\n");
    }
    if (f->used)
    {
      if (f->refbit == 0 && f->p && f->p->qlevel > 1)
      {
        clock_hand = (clock_hand + 1) % MAX_FRAMES;
        return f;
      }
    }

    clock_hand = (clock_hand + 1) % MAX_FRAMES;
  }

  // PASS 2: normal clock
  for (int i = 0; i < MAX_FRAMES * 2; i++)
  {
    struct frame *f = &frame_table[clock_hand];

    // if(f->used){
    if (f->used && f->p)
    {
      if (f->refbit == 0)
      {
        clock_hand = (clock_hand + 1) % MAX_FRAMES;
        return f;
      }
      else
      {
        f->refbit = 0;
      }
    }

    clock_hand = (clock_hand + 1) % MAX_FRAMES;
  }
  return 0;
}

int evict_page()  // evict a page using select_victim and return 1 if successful, 0 if no victim found (shouldn't happen since we only call evict_page when frame table is full)
{
  while (1)
  {
    acquire(&frame_lock);
    struct frame *f = select_victim();

    if (f == 0)  
    {
      release(&frame_lock);
      printf("evict_page: no victim found\n");
      return 0;
    }

    struct proc *p = f->p;
    uint64 va = f->va;
    uint64 pa = f->pa;

    pte_t *pte = walk(p->pagetable, va, 0);
    if (pte == 0 || (*pte & PTE_V) == 0)
    {
      stale_frame_cleanup_count++;
      printf("evict_page: invalid PTE for victim page (va=0x%lx), cleaning up stale frame entry (count=%d)\n", va, stale_frame_cleanup_count);
      f->used = 0;
      f->p = 0;
      f->va = 0;
      f->pa = 0;
      f->refbit = 0;
      release(&frame_lock);
      continue; // Try another victim
    }

    p->vmstats.pages_evicted++;
    p->vmstats.resident_pages--;

    swap_out(p, va, (void *)pa);
    *pte = (*pte & ~PTE_V) | PTE_S;   // mark page as swapped (invalid but in swap)
    sfence_vma();  
    p->vmstats.pages_swapped_out++;

    f->used = 0;
    f->p = 0;
    f->va = 0;
    f->pa = 0;
    f->refbit = 0;
    release(&frame_lock);

    kfree((void *)pa);   

    return 1;
  }
}

uint64 swap_in(struct proc *p, uint64 va)
{
  int vpn = va / PGSIZE;

  //check vpn whether exceed max process pages
  if (vpn >= MAX_PSYC_PAGES)
    return 0;

  //check page swap or not
  if (!p->swapped[vpn])
    return 0;

  int idx = p->swap_index[vpn];
  if (idx < 0 || idx >= MAX_SWAP)
    panic("invalid swap index");

  //make suer the page is not already mapped
  pte_t *pte = walk(p->pagetable, va, 0);
  if (pte == 0)
    return 0;
  int perm = PTE_FLAGS(*pte) & ~(PTE_S | PTE_V);
  *pte =0;
  char *mem = kalloc();
  if (mem == 0)
    return 0;
  // copy page from swap space to physical memory
  memmove(mem, swap_space[idx], PGSIZE);

  acquire(&swap_lock);
  swap_used[idx] = 0;
  release(&swap_lock);

  // update page table to point to new physical memory and mark it valid
  if (mappages(p->pagetable, va, PGSIZE, (uint64)mem,
               perm | PTE_V) != 0)
  {
    kfree(mem);
    return 0;
  }

  p->swapped[vpn] = 0;
  p->swap_index[vpn] = -1;
  sfence_vma();

  add_frame(p, va, (uint64)mem);

  acquire(&p->lock);
  p->vmstats.pages_swapped_in++; // page read from swap
  // p->vmstats.resident_pages++;   // page now resident
  release(&p->lock);

  return (uint64)mem; 
}

int swap_out(struct proc *p, uint64 va, void *pa)
{
  // find a free slot in swap spaceand copy the page there
  acquire(&swap_lock);

  int idx = -1;
  for (int i = 0; i < MAX_SWAP; i++)
  {
    if (!swap_used[i])
    {
      idx = i;
      swap_used[i] = 1;
      break;
    }
  }

  release(&swap_lock);

  if (idx == -1)
    panic("swap full");

  memmove(swap_space[idx], pa, PGSIZE);

  int vpn = va / PGSIZE;
  if (vpn >= MAX_PSYC_PAGES)  // 
    return -1;
  p->swapped[vpn] = 1;
  p->swap_index[vpn] = idx;

  return 0;
}

void free_proc_swap_space(struct proc *p)
{
  // free any swap slots used by this process
  acquire(&swap_lock);
  for (int i = 0; i < MAX_PSYC_PAGES; i++)
  {
    if (p->swapped[i])
    {
      int idx = p->swap_index[i];
      if (idx >= 0 && idx < MAX_SWAP)
        swap_used[idx] = 0;
      p->swapped[i] = 0;
      p->swap_index[i] = -1;
    }
  }
  release(&swap_lock);
}

void free_proc_frames(struct proc *p)
{
  acquire(&frame_lock);
  for (int i = 0; i < MAX_FRAMES; i++)
  {
    if (frame_table[i].used && frame_table[i].p == p)
    {
      frame_table[i].used = 0;
      frame_table[i].p = 0;
      frame_table[i].pa = 0;
      frame_table[i].va = 0;
      frame_table[i].refbit = 0;
      // Note: physical page is already freed in proc_freepagetable
    }
  }
  release(&frame_lock);
}
void set_refbit(uint64 pa)
{
  // pa is a physical address from the page table entry.
  // Convert to kernel virtual address to match frame_table.pa values.
  uint64 kva = pa + KERNBASE;

  acquire(&frame_lock);
  for (int i = 0; i < MAX_FRAMES; i++)
  {
    if (frame_table[i].used && frame_table[i].pa == kva)
    {
      frame_table[i].refbit = 1;
      break;
    }
  }
  release(&frame_lock);
}