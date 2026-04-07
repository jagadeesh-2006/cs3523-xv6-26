#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"



struct vmstats stats;
int main()
{
    int pages = 100;
    char *p = sbrk(pages * 4096);
    int i;

    // 1. Initial Allocation
    for (i = 0; i < pages; i++)
        p[i * 4096] = (char)i;

    printf("Low-priority process (PID %d) allocated %d pages.\n", getpid(), pages);
    
    // 2. WAIT: This is where you should manually run a HIGH PRIORITY process 
    // in another shell, or have a fork() here that uses all memory.
    printf("Sleeping... Run high-priority tasks now to force me out of RAM.\n");
    pause(50); 

    // 3. Re-access: This will force the kernel to swap the pages back IN.
    printf("Checking which pages survived...\n");
    for (i = 0; i < pages; i++) {
        if (p[i * 4096] != (char)i) {
            printf("Error: Data corruption at page %d!\n", i);
        }
    }

    // 4. Get Stats
    if (getvmstats(getpid(), &stats) < 0) {
        printf("Syscall failed\n");
        exit(1);
    }

    printf("\n--- Priority Eviction Stats ---\n");
    printf("Page Faults:       %d\n", stats.page_faults);
    printf("Pages Evicted:     %d\n", stats.pages_evicted);
    printf("Pages Swapped In:  %d\n", stats.pages_swapped_in);
    printf("Resident Pages:    %d\n", stats.resident_pages);

    exit(0);
}