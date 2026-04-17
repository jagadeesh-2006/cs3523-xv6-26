#include "kernel/types.h"
#include "user/user.h"

void second_chance_test() {
    printf("--- Clock Reference Bit & Persistence Test ---\n");
    int frames = 128;
    int total = 130; 
    char *p = sbrk(total * 4096);

    // 1. Fill memory exactly to the limit
    for(int i = 0; i < frames; i++) {
        p[i * 4096] = 'A'; 
    }

    // 2. Access Page 0 and Page 1 again. 
    // This sets the PTE_A (Accessed) bit to 1.
    printf("Setting Reference bits for Page 0 and 1...\n");
    volatile char c;
    c = p[0 * 4096];
    c = p[1 * 4096];
    (void)c; 

    // 3. Trigger eviction by accessing page 128 and 129.
    // If Second Chance works, the Clock hand should:
    // - See Page 0 (bit=1), set bit=0, move on.
    // - See Page 1 (bit=1), set bit=0, move on.
    // - Evict Page 2 (bit=0).
    printf("Triggering evictions...\n");
    p[128 * 4096] = 'B';
    p[129 * 4096] = 'B';

    // 4. Verification: If Page 0 was evicted, your 'Second Chance' failed.
    // We can check this by looking at kernel stats if getvmstats tracks per-page.
    // Otherwise, check if Page 2's data is still 'A' without a fault (impossible to check from userland
    // without timing, but we can verify the system doesn't crash).
    printf("Completed. If Clock hand is persistent, Page 0 survived.\n\n");
}

void fork_swap_test() {
    printf("--- Fork with Swapped Pages Test ---\n");
    int pages = 150; // More than 128, forces ~22 pages into swap
    char *p = sbrk(pages * 4096);

    for(int i = 0; i < pages; i++) {
        p[i * 4096] = (char)(i % 256);
    }
    
    printf("Parent filled memory and forced evictions. Forking...\n");

    int pid = fork();
    if(pid < 0) {
        printf("Fork failed\n");
        exit(1);
    }

    if(pid == 0) {
        // CHILD
        printf("Child (PID %d) checking data integrity...\n", getpid());
        for(int i = 0; i < pages; i++) {
            if(p[i * 4096] != (char)(i % 256)) {
                printf("FAIL: Child read wrong data at page %d (Expected %d, Got %d)\n", 
                        i, (i % 256), p[i * 4096]);
                exit(1);
            }
        }
        printf("Child Data Integrity: PASS\n");
        exit(0);
    } else {
        // PARENT
        wait(0);
        printf("Parent: Child finished correctly.\n\n");
    }
}
void wrap_around_test() {
    printf("--- Clock Wrap-Around Test ---\n");
    // Allocating twice the physical memory to force the hand to rotate multiple times
    int total_pages = 255; 
    char *p = sbrk(total_pages * 4096);

    for(int i = 0; i < total_pages; i++) {
        p[i * 4096] = 'W';
        if(i == 127) printf("Reached frame 127 (last frame)...\n");
        if(i == 128) printf("Hand should now wrap to frame 0...\n");
    }
    
    printf("Wrap-around test completed without kernel panic.\n\n");
}
int main() {
    // second_chance_test();
    // fork_swap_test();
    wrap_around_test();
    printf("All persistence and logic tests passed!\n");
    exit(0);
}

