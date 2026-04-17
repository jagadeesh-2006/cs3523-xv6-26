#include "kernel/types.h"
#include "user/user.h"

int main() {
    printf("Corner Case: Exhausting Total Memory (Frames + Swap)...\n");
    
    // MAX_FRAMES(128) + MAX_SWAP(256) = 384 total possible pages.
    // We request 512 to guarantee failure.
    int total_pages = 512;
    char *p = sbrk(total_pages * 4096);

    for (int i = 0; i < total_pages; i++) {
        p[i * 4096] = 'X';
        if (i % 50 == 0) printf("Allocated %d pages...\n", i);
    }

    printf("If you see this, your memory limit was not enforced!\n");
    exit(0);
}