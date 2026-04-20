// // user/schedtest.c
// #include "kernel/types.h"
// #include "kernel/stat.h"
// #include "user/user.h"
// #include "kernel/proc.h"
// #include "kernel/vm.h"

// // setdisksched and getvmstats syscall numbers must match your syscall table
// // getvmstats is from PA1 — use your existing vmstats struct

// #define NPAGES 160
// void run_test(int policy, const char *name) {
//   setdisksched(policy);

//   // Allocate and access pages in a scattered order to generate many
//   // disk requests at varying block distances — this makes the difference
//   // between FCFS and SSTF visible.
//   char *base = sbrk(NPAGES * 4096);
//   if (base == (char*)-1) { printf("sbrk failed\n"); exit(1); }

//   // Access in a pattern that deliberately jumps around
//   // (even indices first, then odd — creates large head movements under FCFS)
//   for (int i = 0; i < NPAGES; i += 2)
//     base[i * 4096] = (char)i;
//   for (int i = 1; i < NPAGES; i += 2)
//     base[i * 4096] = (char)i;

//   // read back all (forces any remaining swap-ins)
//   int chk = 0;
//   for (int i = 0; i < NPAGES; i++)
//     chk += base[i * 4096];

//   // retrieve per-process disk stats (PA1 integration)
// //   struct vmstats st;
// //   getvmstats(getpid(), &st);

// //   struct proc * p = myproc();

// //   printf("[%s] disk_reads=%ld disk_writes=%ld total_latency=%ld checksum=%d\n",
// //          name,
// //          p->disk_reads,
// //          p->disk_writes,
// //          p->total_disk_latency,
// //          chk);

//   sbrk(-(NPAGES * 4096));   // release memory
// }

// int main() {
//   printf("=== Disk scheduling comparison ===\n");
//   run_test(0, "FCFS");
//   run_test(1, "SSTF");
//   printf("Expected: SSTF total_latency <= FCFS total_latency\n");
//   exit(0);
// }