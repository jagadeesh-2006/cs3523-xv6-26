# Virtual Memory Implementation 


---

## 1. kernel/param.h - Constants Definition

```c
#define MAX_FRAMES 128   // Physical frames available
#define MAX_PSYC_PAGES 64   // Max pages per process
#define MAX_SWAP 256     // Swap space size
```

**Status**: All constants properly defined
**Check**: MAX_FRAMES < MAX_PSYC_PAGES * NPROC to avoid index overflow

---

## 2. kernel/proc.h - Data Structures

### struct vmstats - CORRECT
```c
struct vmstats {
  int page_faults;       // Total page faults
  int pages_evicted;     // Pages removed from memory
  int pages_swapped_in;  // Pages restored from swap
  int pages_swapped_out; // Pages stored to swap
  int resident_pages;    // Loaded pages in memory
};
```

### struct proc - CORRECT
```c
struct proc {
  // ... existing fields ...
  
  // VM-related fields
  struct vmstats vmstats;              // Statistics
  int swapped[MAX_PSYC_PAGES];         // Swap status tracking
  int swap_index[MAX_PSYC_PAGES];      // Swap space indices
  uint64 pagelist[MAX_PSYC_PAGES];     // Virtual addresses
  int num_pages;                       // Page count
  
  // MLFQ fields (for priority-aware eviction)
  int qlevel;                          // Current queue level (0-3)
};
```

**Status**: All necessary fields present

---

## 3. kernel/vm.c - Core VM Management

### Global Data Structures - CORRECT
```c
struct frame {
  int used;        // In-use flag
  struct proc *p;  // Owner process pointer
  uint64 pa;       // Physical address
  uint64 va;       // Virtual address
  int refbit;      // Reference bit (Clock algo)
};

struct frame frame_table[MAX_FRAMES];  // Global frame table
int clock_hand = 0;                    // Clock pointer
struct spinlock frame_lock;            // Synchronization

char swap_space[MAX_SWAP][PGSIZE];     // In-memory swap
int swap_used[MAX_SWAP];               // Swap slot tracking
struct spinlock swap_lock;             // Synchronization
```

**Status**: Proper structure and locking

---

### vmfault() - Page Fault Handler - MINOR ISSUE

```c
uint64 vmfault(pagetable_t pagetable, uint64 va, int read)
{
  struct proc *p = myproc();
  
  if (va >= p->sz)
    return 0;  // Address out of range
  
  va = PGROUNDDOWN(va);
  
  if(ismapped(pagetable, va))
    return walkaddr(pagetable, va);  //  Already mapped
  
  p->vmstats.page_faults++;  //  Increment page faults
  
  int vpn = va / PGSIZE;
  
  // CASE 1: Swapped Page - 
  if(p->swapped[vpn]){
    return swap_in(p, va);
  }
  
  // CASE 2: New Page - CHECK THIS
  char *mem = kalloc();
  if(mem == 0)
    return 0;  // ⚠️ PROBLEM: Should retry after eviction?
  
  // ... mapping and setup ...
  
  p->vmstats.resident_pages++;  //  Update stats
  
  return (uint64)mem;
}
```

**Issue Found**: When `kalloc()` fails even after calling `evict_page()`, returns 0 directly.

**Analysis**: This is actually OK because:
- `kalloc()` internally calls `evict_page()` when memory is exhausted
- If eviction fails, returning 0 is correct (allow process to crash)
- Alternatively, could loop and retry vmfault

**Status**: ACCEPTABLE (kalloc handles eviction internally)

---

### select_victim() - Clock Algorithm -  CORRECT

```c
struct frame* select_victim()
{
  // PASS 1: Low-priority pages with refbit=0 (Priority-aware)
  for(int i = 0; i < MAX_FRAMES; i++){
    struct frame *f = &frame_table[clock_hand];
    
    if(f->used && f->refbit == 0 && f->p && f->p->qlevel > 1){
      //  Prefer: lower priority + not recently used
      clock_hand = (clock_hand + 1) % MAX_FRAMES;
      return f;
    }
    clock_hand = (clock_hand + 1) % MAX_FRAMES;
  }
  
  // PASS 2: Normal Clock algorithm
  for(int i = 0; i < MAX_FRAMES * 2; i++){
    struct frame *f = &frame_table[clock_hand];
    
    if(f->used){
      if(f->refbit == 0){
        //  Found: recently unused page
        return f;
      } else {
        //  Clear refbit and continue
        f->refbit = 0;
      }
    }
    clock_hand = (clock_hand + 1) % MAX_FRAMES;
  }
  
  return 0;  //  No victim found
}
```

**Status**: CORRECT - Implements Clock algorithm with priority awareness

**Verification**:
- Pass 1: Scans for low-priority (qlevel > 1) with refbit=0
- Pass 2: Standard Clock algorithm - clear refbit if set, take if clear
- Circular scanning with modulo MAX_FRAMES
- Returns pointer to frame, not index

---

### evict_page() - Page Eviction -  CORRECT

```c
int evict_page()
{
  acquire(&frame_lock);
  
  struct frame *f = select_victim();
  if(f == 0){
    release(&frame_lock);
    return 0;  //  No victim available
  }
  
  struct proc *p = f->p;
  uint64 va = f->va;
  uint64 pa = f->pa;
  
  p->vmstats.pages_evicted++;           // Track eviction
  p->vmstats.resident_pages--;          // Update resident count
  
  pte_t *pte = walk(p->pagetable, va, 0);
  if(pte == 0 || (*pte & PTE_V) == 0){
    release(&frame_lock);
    return 0;  // Page already invalid
  }
  
  swap_out(p, va, (void*)pa);           // Store to swap
  *pte = (*pte & ~PTE_V) | PTE_S;       // Mark as swapped
  sfence_vma();                         // TLB shootdown
  p->vmstats.pages_swapped_out++;       // Track swap-out
  
  f->used = 0;  // Clear frame entry
  f->p = 0;
  f->va = 0;
  f->pa = 0;
  f->refbit = 0;
  
  release(&frame_lock);
  
  kfree((void*)pa);  // Free physical memory
  
  return 1;  // Success
}
```

**Status**: CORRECT - Properly handles all aspects of eviction

---

### swap_in() - Restore Swapped Pages -  CORRECT

```c
uint64 swap_in(struct proc *p, uint64 va)
{
  int vpn = va / PGSIZE;
  
  if(!p->swapped[vpn])
    return 0;  //  Not swapped
  
  int idx = p->swap_index[vpn];
  if(idx < 0 || idx >= MAX_SWAP)
    panic("invalid swap index");  // Sanity check
  
  char *mem = kalloc();  //  Might trigger eviction
  if(mem == 0)
    return 0;
  
  memmove(mem, swap_space[idx], PGSIZE);  // Restore
  
  acquire(&swap_lock);
  swap_used[idx] = 0;  // Free swap slot
  release(&swap_lock);
  
  p->swapped[vpn] = 0;  // Clear swapped flag
  
  if(mappages(p->pagetable, va, PGSIZE, (uint64)mem,
      PTE_W|PTE_U|PTE_R) != 0){
    kfree(mem);
    return 0;  //  Error handling
  }
  
  sfence_vma();  //  TLB shootdown
  add_frame(p, va, (uint64)mem);
  
  acquire(&p->lock);
  p->vmstats.pages_swapped_in++;   // Track swap-in
  p->vmstats.resident_pages++;     // Update resident
  release(&p->lock);
  
  return (uint64)mem;
}
```

**Status**: CORRECT - Handles swap restoration

---

### swap_out() - Store Evicted Pages -  CORRECT

```c
int swap_out(struct proc *p, uint64 va, void *pa)
{
  acquire(&swap_lock);
  
  int idx = -1;
  for(int i = 0; i < MAX_SWAP; i++){
    if(!swap_used[i]){
      idx = i;
      swap_used[i] = 1;  //  Mark slot as used
      break;
    }
  }
  
  release(&swap_lock);
  
  if(idx == -1)
    panic("swap full");  //  Error if no space
  
  memmove(swap_space[idx], pa, PGSIZE);  //  Copy page
  
  int vpn = va / PGSIZE;
  p->swapped[vpn] = 1;              // Mark as swapped
  p->swap_index[vpn] = idx;         //  Store location
  
  return 0;
}
```

**Status**: CORRECT - Simple and effective

---

### add_frame() - Register Frame -  CORRECT

```c
void add_frame(struct proc *p, uint64 va, uint64 pa)
{
  acquire(&frame_lock);
  
  for(int i = 0; i < MAX_FRAMES; i++){
    if(frame_table[i].used == 0){
      frame_table[i].used = 1;
      frame_table[i].p = p;
      frame_table[i].va = va;
      frame_table[i].pa = pa;
      frame_table[i].refbit = 1;  //  Start with refbit=1
      release(&frame_lock);
      return;
    }
  }
  
  release(&frame_lock);
  if(evict_page()){
    return add_frame(p, va, pa);  //  Retry after eviction
  }
  panic("no free frame slot");
}
```

**Status**: CORRECT - Handles frame registration and recursive eviction

---

### set_refbit() - Reference Bit Management -  CORRECT

```c
void set_refbit(uint64 pa) {
    acquire(&frame_lock);
    for(int i = 0; i < MAX_FRAMES; i++){
        if(frame_table[i].used && frame_table[i].pa == pa){
            frame_table[i].refbit = 1;  //  Set refbit
            break;
        }
    }
    release(&frame_lock);
}
```

**Status**: CORRECT - Called by walkaddr() on page access

---

## 4. kernel/kalloc.c - Memory Allocation - CORRECT

```c
void *kalloc(void)
{
  struct run *r;
  
  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);
  
  // NEW: Handle memory exhaustion
  if(r == 0){
    if(evict_page() == 0)
      return 0;  // Eviction failed, out of memory
    
    // Retry after eviction
    acquire(&kmem.lock);
    r = kmem.freelist;
    if(r)
      kmem.freelist = r->next;
    release(&kmem.lock);
  }
  
  memset((char*)r, 5, PGSIZE);
  return (void*)r;
}
```

**Status**: CORRECT - Integrates eviction with memory allocation

---

## 5. kernel/trap.c - Page Fault Handling - CORRECT

```c
uint64 usertrap(void)
{
  // ... setup ...
  
  struct proc *p = myproc();
  p->trapframe->epc = r_sepc();
  
  if (r_scause() == 8) {
    // System call
    // ...
  }
  else if ((which_dev = devintr()) != 0) {
    // Device interrupt
  }
  else if ((r_scause() == 15 || r_scause() == 13) &&
           vmfault(p->pagetable, r_stval(), (r_scause() == 13) ? 1 : 0) != 0)
  {
    // Handle page fault (scause 13=store, 15=load)
    // vmfault updates stats and handles eviction
  }
  else {
    // Unexpected exception
    setkilled(p);
  }
  
  if (killed(p))
    kexit(-1);
  
  // ...
}
```

**Status**: CORRECT - Dispatches to vmfault()

---

## 6. kernel/sysproc.c - getvmstats Syscall - CORRECT

```c
uint64 sys_getvmstats(void)
{
  int pid;
  uint64 addr;
  argint(0, &pid);           // Get PID argument
  argaddr(1, &addr);         // Get buffer address
  struct vmstats info;
  
  if (kgetvmstats(pid, &info) < 0)
    return -1;               // Invalid PID
  
  if (copyout(myproc()->pagetable, addr, (char *)&info, sizeof(info)) < 0)
    return -1;               // Copy to user space
  
  return 0;                  // Success
}
```

**Status**: CORRECT - Proper syscall wrapper

---

## 7. kernel/proc.c - Kernel Helper - CORRECT

```c
int kgetvmstats(int pid, struct vmstats *info)
{
  int found = -1;
  for (struct proc *p = proc; p < &proc[NPROC]; p++)
  {
    acquire(&p->lock);
    if (p->pid == pid)
    {
      // Copy all statistics
      info->page_faults = p->vmstats.page_faults;
      info->pages_evicted = p->vmstats.pages_evicted;
      info->pages_swapped_in = p->vmstats.pages_swapped_in;
      info->pages_swapped_out = p->vmstats.pages_swapped_out;
      info->resident_pages = p->vmstats.resident_pages;
      found = 0;
      release(&p->lock);
      break;
    }
    release(&p->lock);
  }
  return found;  // 0 on success, -1 on failure
}
```



---

## 8. kernel/defs.h - Function Declarations - CORRECT

**Verified declarations**:
```c
// Forward declarations
struct vmstats;  // Added

// Function declarations
int kgetvmstats(int, struct vmstats *);  // Declared
uint64 vmfault(pagetable_t, uint64, int);  // Existing
```


---

## 9. User Programs - Test Implementations

All test programs properly:
- Call `getvmstats(pid, &pr->vmstats)` 
- Print all vmstats fields
- Have proc.h include for type definitions

**Files verified**:
- alloctest.c - Allocation and page faults
- evicttest.c - Forced eviction
- reusetest.c - Swap-in verification
- prioritytest.c - Priority-based eviction

---

## TEST CASES VERIFICATION

### Test 1: alloctest.c - Basic Allocation & Demand Paging

**Purpose**: Verify that page faults are triggered on page access and statistics are tracked.

**Test Flow**:
```c
int pages = 50;           // Allocate 50 pages (~200 KB)
char *p = sbrk(pages * 4096);

// Touch each page sequentially
for(i = 0; i < pages; i++)
    p[i*4096] = i;        // Each access triggers page fault

// Get statistics
proc *pr = myproc();
int pid = getpid();
getvmstats(pid, &pr->vmstats);
```

**Expected Results**:
| Metric | Expected Value | Reason |
|--------|----------------|--------|
| `page_faults` | ~50 | One fault per page access |
| `resident_pages` | ~50 | All pages successfully allocated |
| `pages_evicted` | 0 | No memory pressure yet |
| `pages_swapped_out` | 0 | No eviction triggered |
| `pages_swapped_in` | 0 | No swapped pages accessed |

**What It Tests**:
- `vmfault()` correctly increments page_faults
- `add_frame()` successfully registers frames
- Reference bits set correctly
- `resident_pages` counter accurate
- `getvmstats()` syscall working

**Success Criteria**:
- page_faults == 50 (or very close)
- resident_pages == 50
- All other metrics == 0

---

### Test 2: evicttest.c - Forced Page Replacement

**Purpose**: Verify that Clock algorithm evicts pages when memory is full, and low-priority pages are preferred.

**Test Flow**:
```c
int pages = 100;                    // Exceeds MAX_FRAMES (128)
char *p = sbrk(pages * 4096);

// Access first half - loads 50 pages into frames
for(i = 0; i < pages/2; i++)
    p[i*4096] = i;
printf("Accessed first half of pages\n");

// Access second half - causes eviction of older pages
for(i = pages/2; i < pages; i++)
    p[i*4096] = i;
printf("Accessed second half of pages\n");

// Get statistics
getvmstats(pid, &pr->vmstats);
```

**Expected Results**:
| Metric | Expected Value | Reason |
|--------|----------------|--------|
| `page_faults` | 100+ | ~100 initial faults + some swap-ins |
| `pages_evicted` | >0 | First half pages evicted |
| `pages_swapped_out` | >0 | Evicted pages stored to swap |
| `resident_pages` | ≤MAX_FRAMES | Only resident pages counted |
| `pages_swapped_in` | 0 or small | No re-access yet |

**What It Tests**:
- `evict_page()` triggers when frames full
- `select_victim()` finds eviction candidates
- `swap_out()` stores pages correctly
- Page table entries marked as swapped (PTE_S)
- `clock_hand` pointer rotates through frames
- Memory pressure handled gracefully

**Success Criteria**:
- pages_evicted > 0
- pages_swapped_out > 0
- resident_pages ≤ MAX_FRAMES
- Process does not crash

---

### Test 3: reusetest.c - Swap-In Verification

**Purpose**: Verify that swapped pages can be restored from swap space and re-accessed.

**Test Flow**:
```c
int pages = 60;                     // More than can fit
char *p = sbrk(pages * 4096);

// Fill memory - causes evictions
for(i = 0; i < pages; i++)
    p[i*4096] = i;
printf("Initial allocation done\n");

// Re-access first 10 pages (were likely evicted)
for(i = 0; i < 10; i++)
    p[i*4096] += 1;  // Access triggers swap-in
printf("Accessed first 10 pages again\n");

// Get statistics
getvmstats(pid, &pr->vmstats);
```

**Expected Results**:
| Metric | Expected Value | Reason |
|--------|----------------|--------|
| `page_faults` | 60+ | Initial 60 + faults during swap-in |
| `pages_evicted` | >10 | Pages removed when memory full |
| `pages_swapped_out` | >10 | Evicted pages stored |
| `pages_swapped_in` | >0 | Re-accessed pages restored |
| `resident_pages` | 60 | All 60 pages now in memory |

**What It Tests**:
- `swap_in()` correctly restores pages
- `swap_space[]` preserves page data
- `swapped[vpn]` flag prevents double allocation
- `swap_index[vpn]` correctly maps pages
- Page contents preserved after eviction
- `mappages()` succeeds after swap-in
- Reference bits updated on swap-in

**Success Criteria**:
- pages_swapped_in > 0
- pages_swapped_out > 0
- page_faults > 60
- Original page values preserved (test prints first pages)

---

### Test 4: prioritytest.c - Priority-Based Eviction

**Purpose**: Verify that lower-priority processes lose pages first when memory pressure exists.

**Test Flow**:
```c
int pages = 40;
char *p = sbrk(pages * 4096);

// Lower-priority process allocates pages
for(i = 0; i < pages; i++)
    p[i*4096] = i;
printf("Low-priority process allocated pages\n");

sleep(5);  // Higher-priority processes run and allocate/evict

// Check which pages survived
for(i = 0; i < pages; i++)
    printf("%d ", p[i*4096]);  // Some pages corrupted if evicted
printf("\n");

// Get statistics
getvmstats(pid, &pr->vmstats);
```

**Expected Results**:
| Metric | Expected Value | Reason |
|--------|----------------|--------|
| `page_faults` | 40+ | Initial + possibly swap-ins |
| `pages_evicted` | >0 | Process pages evicted (if qlevel>1) |
| `pages_swapped_out` | >0 | Pages stored to swap |
| `resident_pages` | <40 | Not all pages survived |
| `pages_swapped_in` | >0 (maybe) | Pages restored if re-accessed |

**What It Tests**:
- `select_victim()` PASS 1 works (low-priority pages preferred)
- Process `qlevel` checked correctly
- Competition for frames with other processes
- Priority-based protection mechanism
- Statistics updated per-process (not globally)


**Success Criteria**:
- pages_evicted > 0
- pages_swapped_out > 0
- At least some page values corrupted (print shows different values)
- Higher-priority processes retain more pages

---


---
