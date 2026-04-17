
#include "kernel/types.h"
#include "user/user.h"


struct vmstats stats;
void print_stats(int pid, char *label) {
    if(getvmstats(pid, &stats) == 0) {
        printf("%s (PID %d) -> Faults: %d, Evicted: %d, Res: %d\n", 
               label, pid, stats.page_faults, stats.pages_evicted, stats.resident_pages);
    }
}

void child_stats_test() {
    printf("--- Child Memory Stats Isolation Test ---\n");
    
    int pages = 50;
    char *p = sbrk(pages * 4096);
    
    // Parent touches memory to establish baseline
    for(int i = 0; i < pages; i++) p[i * 4096] = 'P';
    
    printf("Parent baseline before fork:\n");
    print_stats(getpid(), "Parent");

    int pid = fork();
    if(pid < 0) exit(1);

    if(pid == 0) {
        // CHILD
        printf("\nChild (PID %d) stats immediately after fork:\n", getpid());
        print_stats(getpid(), "Child");

        printf("Child triggering 20 new page faults...\n");
        char *p2 = sbrk(20 * 4096);
        for(int i = 0; i < 20; i++) p2[i * 4096] = 'C';

        printf("Child stats after activity:\n");
        print_stats(getpid(), "Child");
        exit(0);
    } else {
        // PARENT
        wait(0);
        printf("\nParent stats after child exited (should NOT have child's 20 faults):\n");
        print_stats(getpid(), "Parent");
    }
}

int main() {
    child_stats_test();
    exit(0);
}