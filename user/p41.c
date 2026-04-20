// user/swaptest.c
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define NPAGES   200          // more than MAX_FRAMES (128) to force evictions
#define PGSIZE   4096

int main() {
  printf("[swaptest] allocating %d pages...\n", NPAGES);

  // allocate NPAGES worth of memory
  char *base = sbrk(NPAGES * PGSIZE);
  if (base == (char*)-1) {
    printf("sbrk failed\n");
    exit(1);
  }

  printf("[swaptest] writing patterns...\n");
  // write a unique pattern to each page
  for (int i = 0; i < NPAGES; i++) {
    char *p = base + i * PGSIZE;
    for (int j = 0; j < PGSIZE; j++)
      p[j] = (char)((i + j) & 0xFF);
  }

  printf("[swaptest] verifying all %d pages...\n", NPAGES);
  int errors = 0;
  for (int i = 0; i < NPAGES; i++) {
    char *p = base + i * PGSIZE;
    for (int j = 0; j < PGSIZE; j++) {
      if (p[j] != (char)((i + j) & 0xFF)) {
        printf("ERROR: page %d offset %d: got %d expected %d\n",
               i, j, (unsigned char)p[j], (unsigned char)((i+j)&0xFF));
        errors++;
        if (errors > 5) goto done;
      }
    }
  }

done:
  if (errors == 0)
    printf("[swaptest] all pages correct — swap-in/out working\n");
  else
    printf("[swaptest] FAILED with %d errors\n", errors);

  exit(0);
}