// user/raid0test.c
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define NPAGES 40

int main() {
  // RAID 0 is the default (current_raid_level=0 at boot)
  printf("[raid0test] write %d pages via RAID 0 swap...\n", NPAGES);

  char *base = sbrk(NPAGES * 4096);
  if (base == (char*)-1) { printf("sbrk failed\n"); exit(1); }

  for (int i = 0; i < NPAGES; i++) {
    char *p = base + i * 4096;
    for (int j = 0; j < 4096; j++)
      p[j] = (char)(i * 7 + j);    // distinct pattern per page
  }

  printf("[raid0test] read back and verify...\n");
  // Touch other memory to evict test pages, then re-access
  char *tmp = sbrk(150 * 4096);
  for (int i = 0; i < 150; i++) tmp[i * 4096] = 0;
  sbrk(-(150 * 4096));

  int errors = 0;
  for (int i = 0; i < NPAGES; i++) {
    char *p = base + i * 4096;
    for (int j = 0; j < 4096; j++) {
      if (p[j] != (char)(i * 7 + j)) {
        errors++;
        if (errors == 1)
          printf("ERROR at page %d offset %d\n", i, j);
      }
    }
  }

  if (errors == 0)
    printf("[raid0test] RAID 0 striping correct\n");
  else
    printf("[raid0test] FAILED: %d byte errors\n", errors);

  exit(0);
}