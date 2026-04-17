#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// struct vmstats {
//     int page_faults;
//     int pages_evicted;
//     int pages_swapped_in;
//     int pages_swapped_out;
//     int resident_pages;
// };

void thrash_test() {
    int pages = 200; // Greater than MAX_FRAMES (128)
    char *p = sbrk(pages * 4096);
    struct vmstats stats;

    printf("Starting Thrash Test: Allocating %d pages...\n", pages);

    // 1. Write data to trigger initial faults and evictions
    for (int i = 0; i < pages; i++) {
        p[i * 4096] = (char)(i % 256);
    }

    // 2. Re-read data to trigger swap-ins
    printf("Re-accessing all pages to trigger swap-ins...\n");
    for (int i = 0; i < pages; i++) {
        if (p[i * 4096] != (char)(i % 256)) {
            printf("FAIL: Data corruption at page %d\n", i);
            exit(1);
        }
    }

    if (getvmstats(getpid(), &stats) == 0) {
        printf("\n--- Thrash Test Stats ---\n");
        printf("Faults: %d, Evicted: %d, Swapped In: %d, Resident: %d\n",
               stats.page_faults, stats.pages_evicted, 
               stats.pages_swapped_in, stats.resident_pages);
        
        if (stats.pages_evicted > 0 && stats.resident_pages <= 128) {
            printf("RESULT: PASS\n");
        } else {
            printf("RESULT: FAIL (Check eviction logic)\n");
        }
    }
}

int main() {
    thrash_test();
    exit(0);
}