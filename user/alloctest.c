#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/vm.h"
struct vmstats stats;
int main() {
    // 1. Define 'pages' to be significantly larger than your MAX_FRAMES
    // If MAX_FRAMES is 30, 100 pages (400KB) is plenty.
    int pages = 200; 
    int i;

    printf("Starting Page Replacement Test...\n");

    // 2. Allocate memory
    char *p = sbrk(pages * 4096);
    if (p == (char*)-1) {
        printf("sbrk failed\n");
        exit(1);
    }

    // 3. Access pages to trigger faults and evictions
    // We write to each page to ensure it is "dirty" and must be swapped out
    for (i = 0; i < pages; i++) {
        p[i * 4096] = (char)(i % 256);
        if (i % 20 == 0) {
            printf("Accessed %d pages...\n", i);
        }
    }

    // 4. Retrieve stats via your custom system call
    int pid = getpid();
    if (getvmstats(pid, &stats) < 0) {
        printf("Error: getvmstats system call failed\n");
        exit(1);
    }

    // 5. Final Report
    printf("\n--- Test Results ---\n");
    printf("Total Pages Requested: %d\n", pages);
    printf("Page Faults:           %d\n", stats.page_faults);
    printf("Pages Evicted:         %d\n", stats.pages_evicted);
    printf("Pages Swapped Out:     %d\n", stats.pages_swapped_out);

    if (stats.pages_evicted > 0) {
        printf("\nSUCCESS: Page replacement was triggered.\n");
    } else {
        printf("\nFAILURE: No pages were evicted. Check MAX_FRAMES.\n");
    }

    exit(0);
}