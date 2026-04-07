#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"


/* * NOTE: Ensure this struct exactly matches the one in your kernel.
 * If you haven't added it to user.h, define it here:
 */

struct vmstats stats;
int main() {
    int pages = 200; //
    int i;
    int pid = getpid();

    // 1. Allocate 60 pages (approx 240KB)
    char *p = sbrk(pages * 4096);
    if (p == (char*)-1) {
        printf("sbrk failed\n");
        exit(1);
    }

    // 2. Fill memory: This triggers initial allocation and faults
    for (i = 0; i < pages; i++) {
        p[i * 4096] = (char)i;
    }
    printf("Initial allocation of %d pages done.\n", pages);

    // 3. Access first 10 pages again: 
    // If your MAX_FRAMES is 30, these were evicted during step 2.
    // Accessing them now MUST trigger swap_in.
    for (i = 0; i < 10; i++) {
        p[i * 4096] += 1; 
    }
    printf("Re-accessed first 10 pages.\n\n");

    // 4. Retrieve stats via syscall
    if (getvmstats(pid, &stats) < 0) {
        printf("Error: getvmstats failed. Check your syscall implementation.\n");
        exit(1);
    }

    // 5. Final Verification Logic
    printf("--- Verification Results ---\n");
    printf("Page Faults:       %d (Expected: ~70)\n", stats.page_faults);
    printf("Pages Evicted:     %d (Expected: >0)\n", stats.pages_evicted);
    printf("Pages Swapped In:  %d (Expected: 10)\n", stats.pages_swapped_in);
    printf("Pages Swapped Out: %d (Expected: >0)\n", stats.pages_swapped_out);
    printf("Resident Pages:    %d (Expected: <= MAX_FRAMES)\n", stats.resident_pages);

    exit(0);
}