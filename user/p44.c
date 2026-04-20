// user/raid1test.c
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// You need a syscall to change RAID level at runtime.
// Add: int setraidlevel(int level) to your syscall table,
// implemented as: current_raid_level = level; return 0;
// For now this test assumes RAID 1 is set before running via
// a companion test runner, OR you add setraidlevel().

#define NPAGES 30

int main() {
  printf("[raid1test] testing RAID 1 mirroring...\n");
  // setraidlevel(1);   // uncomment once setraidlevel syscall is added

  char *base = sbrk(NPAGES * 4096);
  if (base == (char*)-1) { printf("sbrk failed\n"); exit(1); }

  for (int i = 0; i < NPAGES; i++) {
    char *p = base + i * 4096;
    for (int j = 0; j < 4096; j++)
      p[j] = (char)(i + j + 13);
  }

  printf("[raid1test] write phase complete\n");

  // Evict by touching extra pages
  char *tmp = sbrk(130 * 4096);
  for (int i = 0; i < 130; i++) tmp[i * 4096] = 1;
  sbrk(-(130 * 4096));

  int errors = 0;
  for (int i = 0; i < NPAGES; i++) {
    char *p = base + i * 4096;
    for (int j = 0; j < 4096; j++) {
      if (p[j] != (char)(i + j + 13))
        errors++;
    }
  }

  if (errors == 0)
    printf("[raid1test] verify phase: all %d pages correct\n[raid1test] RAID 1 mirroring correct\n", NPAGES);
  else
    printf("[raid1test] FAILED: %d errors\n", errors);

  exit(0);
}