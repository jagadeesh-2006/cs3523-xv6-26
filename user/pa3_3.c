// Credit: Akshat
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main() {
    printf("\n--- Test 3: Scheduler-Aware Eviction ---\n");
    
    int fd_B_to_A[2], fd_A_to_B[2];
    if(pipe(fd_B_to_A) < 0 || pipe(fd_A_to_B) < 0) exit(1);

    int pid_B = fork();
    if (pid_B == 0) {
        // CHILD B: IO Bound (Level 0)
        int pgsz = 4096;
        int pages = 8000; // Safe footprint
        char *mem = sbrk(pages * pgsz);
        printf("going to loop\n");
        for(int i = 0; i < pages; i++) mem[i * pgsz] = 'B';
        printf("loop done\n");
        
        int mypid = getpid();
        write(fd_B_to_A[1], &mypid, sizeof(mypid));
        
        char buf[1];
        read(fd_A_to_B[0], buf, 1); // Blocks -> Level 0
        exit(0);
    }

    int pid_A = fork();
    if (pid_A == 0) {
        // CHILD A: CPU Bound (Level 3)
        int child_b_pid;
        read(fd_B_to_A[0], &child_b_pid, sizeof(child_b_pid));
        
        // Setup a massive target footprint BEFORE spinning
        int pgsz = 4096;
        int initial_pages = 18000;
        printf("Child A: Allocating %d pages to set up frame table...\n", initial_pages);
        char *base_mem = sbrk(initial_pages * pgsz);
        printf("Child A: Allocation done. Touching pages to fault them in...\n");
        for(int i=0; i<initial_pages; i++) base_mem[i*pgsz] = 'A';
        
        // Spin heavily to drop to Level 3 priority
        printf("Child A: Spinning to drop to Level 3 priority...\n");
        for( int i=0; i<500000000; i++); 
        printf("Child A: Spinning done. Now pushing for evictions...\n");
        
        struct vmstats st_A, st_B;
        
        // Push the system over the edge to trigger evictions!
        for(int i = 0; i < 5000; i++) { 
            char *mem = sbrk(pgsz);
            if(mem == (char*)-1) break;
            mem[0] = 'X'; 
            
            getvmstats(getpid(), &st_A);
            getvmstats(child_b_pid, &st_B);
            
            if((st_A.pages_evicted + st_B.pages_evicted) >= 50) {
                break; 
            }
        }
        
        getvmstats(getpid(), &st_A);
        getvmstats(child_b_pid, &st_B);
        
        printf("Child A (CPU Bound) Evictions: %d\n", st_A.pages_evicted);
        printf("Child B (IO Bound) Evictions: %d\n", st_B.pages_evicted);
        
        write(fd_A_to_B[1], "y", 1);
        exit(0);
    }

    close(fd_B_to_A[0]); close(fd_B_to_A[1]);
    close(fd_A_to_B[0]); close(fd_A_to_B[1]);
    wait(0);
    wait(0);
    
    printf("Test 3 Complete. (Child A should have more evictions than Child B)\n\n");
    exit(0);
}