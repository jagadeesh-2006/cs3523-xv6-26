#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"


struct vmstats stats;

int main()
{
    // 1. Setup: Request 100 pages. 
    // If your MAX_FRAMES is small (e.g., 30), this will force evictions.
    int pages = 100; 
    char *p = sbrk(pages * 4096);
    int i;

    if (p == (char*)-1) {
        printf("sbrk failed\n");
        exit(1);
    }

    // 2. Access first half: This triggers page faults and populates physical memory.
    for (i = 0; i < pages / 2; i++)
        p[i * 4096] = (char)i;

    printf("Accessed first half of pages (%d pages)\n", pages / 2);

    // 3. Access second half: This should trigger evictions of the first half 
    // once physical memory (MAX_FRAMES) is exceeded.
    for (i = pages / 2; i < pages; i++)
        p[i * 4096] = (char)i;

    printf("Accessed second half of pages (%d pages)\n", pages / 2);

    // 4. Retrieve stats: Use the system call you implemented.
    // We pass the address of our local 'stats' struct.
    if ((getvmstats(getpid(), &stats) ) < 0) {
        printf("Error: getvmstats failed\n");
        exit(1);
    }

    // 5. Results Display
    printf("\n--- Virtual Memory Stats ---\n");
    printf("Page Faults:       %d\n", stats.page_faults);
    printf("Pages Evicted:     %d\n", stats.pages_evicted);
    printf("Pages Swapped In:  %d\n", stats.pages_swapped_in);
    printf("Pages Swapped Out: %d\n", stats.pages_swapped_out);
    printf("Resident Pages:    %d\n", stats.resident_pages);

    exit(0);
}