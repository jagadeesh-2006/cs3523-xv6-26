#include "kernel/types.h"
#include "user/user.h"

// Note: Ensure your getvmstats struct matches the kernel definition
// struct vmstats {
//     int page_faults;
//     int pages_evicted;
//     int pages_swapped_in;
//     int pages_swapped_out;
//     int resident_pages;
// };

void priority_test() {
    int pid = fork();

    if (pid == 0) {
        // CHILD: Low Priority
        // Use a system call from PA2 to set priority to lowest (e.g., setpriority(2))
        // set_priority(2); 

        char *p = sbrk(100 * 4096);
        for(int i=0; i<100; i++) p[i*4096] = 'L';
        
        printf("Low priority child (PID %d) filled 100 pages. pauseing...\n", getpid());
        pause(50); // Wait for parent to hog memory

        struct vmstats s;
        getvmstats(getpid(), &s);
        printf("Low Priority Stats: Resident: %d, Evicted: %d\n", s.resident_pages, s.pages_evicted);
        exit(0);
    } else {
        // PARENT: High Priority
        pause(10); // Let child allocate first
        printf("High priority parent (PID %d) starting allocation...\n", getpid());
        
        char *p = sbrk(100 * 4096);
        for(int i=0; i<100; i++) p[i*4096] = 'H';

        wait(0);
        struct vmstats s;
        getvmstats(getpid(), &s);
        printf("High Priority Stats: Resident: %d, Evicted: %d\n", s.resident_pages, s.pages_evicted);
    }
}

int main() {
    priority_test();
    exit(0);
}