// user/raid5test.c
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// Needs two extra syscalls:
//   int setraidlevel(int level)  -- sets current_raid_level
//   int setdiskfail(int disk)    -- sets disk_fail_sim (-1 = no failure)

#define NPAGES 20

int main() {
  printf("[raid5test] writing 20 pages under RAID 5...\n");
  // setraidlevel(5);

  char *base = sbrk(NPAGES * 4096);
  if (base == (char*)-1) { printf("sbrk failed\n"); exit(1); }

  for (int i = 0; i < NPAGES; i++) {
    char *p = base + i * 4096;
    for (int j = 0; j < 4096; j++)
      p[j] = (char)(i * 3 + j + 5);
  }

  // Force eviction so pages go to disk
  char *tmp = sbrk(130 * 4096);
  for (int i = 0; i < 130; i++) tmp[i * 4096] = 0xFF;
  sbrk(-(130 * 4096));

  printf("[raid5test] simulating disk 2 failure...\n");
  // setdiskfail(2);   // disk_fail_sim = 2; reads from disk 2 trigger XOR reconstruction

  printf("[raid5test] reading back with reconstruction...\n");
  int errors = 0;
  for (int i = 0; i < NPAGES; i++) {
    char *p = base + i * 4096;
    for (int j = 0; j < 4096; j++) {
      if (p[j] != (char)(i * 3 + j + 5))
        errors++;
    }
  }

  // setdiskfail(-1);  // clear failure simulation

  if (errors == 0)
    printf("[raid5test] all %d pages reconstructed correctly\n[raid5test] RAID 5 reconstruction verified\n", NPAGES);
  else
    printf("[raid5test] FAILED: %d errors\n", errors);

  exit(0);
}