#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "vm.h"

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  kexit(n);
  return 0; // not reached
}
uint64
sys_getvmstats(void)
{
  int pid;
  uint64 addr;
  argint(0, &pid);
  argaddr(1, &addr);
  struct vmstats info;
  if (kgetvmstats(pid, &info) < 0)
    return -1;
  if (copyout(myproc()->pagetable, addr, (char *)&info, sizeof(info)) < 0)
    return -1;
  return 0;
}
uint64
sys_getpid(void)
{
  return myproc()->pid;
}
uint64
sys_getppid(void)
{
  struct proc *p = myproc();
  return kgetppid(p);
}

uint64
sys_getnumchild(void)
{
  // struct proc *p = myproc();
  return kgetnumchild();
}

uint64
sys_getsyscount(void)
{
  struct proc *p = myproc();
  if (p == 0)
    return -1;
  acquire(&p->lock);
  int cnt = p->syscallcount;
  release(&p->lock);
  return cnt;
}

// system call count of a child process with the given PID.
uint64
sys_childsyscount(void)
{
  int pid;
  argint(0, &pid);
  return kchildsyscount(pid);
}

// return current MLFQ level of calling process
uint64
sys_getlevel(void)
{
  struct proc *p = myproc();
  if (!p)
    return -1;
  return p->qlevel;
}

// fill user-provided mlfqinfo struct with stats for process pid
uint64
sys_getmlfqinfo(void)
{
  int pid;
  uint64 addr;
  argint(0, &pid);
  argaddr(1, &addr);
  struct mlfqinfo info;
  if (kgetmlfqinfo(pid, &info) < 0)
    return -1;
  if (copyout(myproc()->pagetable, addr, (char *)&info, sizeof(info)) < 0)
    return -1;
  return 0;
}
uint64
sys_fork(void)
{
  return kfork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return kwait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int t;
  int n;

  argint(0, &n);
  argint(1, &t);
  addr = myproc()->sz;

  // if (t == SBRK_EAGER || n < 0)
  if(n<0)
  {
    if (growproc(n) < 0)
    {
      return -1;
    }
  }
  else
  {
    // Lazily allocate memory for this process: increase its memory
    // size but don't allocate memory. If the processes uses the
    // memory, vmfault() will allocate it.
    if (addr + n < addr)
      return -1;
    if (addr + n > TRAPFRAME)
      return -1;
    myproc()->sz += n;
  }
  return addr;
}

uint64
sys_pause(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  if (n < 0)
    n = 0;
  acquire(&tickslock);
  ticks0 = ticks;
  while (ticks - ticks0 < n)
  {
    if (killed(myproc()))
    {
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kkill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}
