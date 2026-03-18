#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main(int argc, char *argv[])
{
    int pid = getpid();
    printf("Process %d starting\n", pid);

    // Get initial level
    int level = getlevel();
    printf("Initial level: %d\n", level);

    // Do some work to potentially demote
    volatile long sum = 0;
    for (long i = 0; i < 100000000; i++)
    {
        sum += i; // Add side effect to prevent compiler optimization
    }

    level = getlevel();
    printf("Level after work: %d\n", level);

    // Get MLFQ info
    struct mlfqinfo info;
    if (getmlfqinfo(pid, &info) == 0)
    {
        printf("MLFQ info for pid %d:\n", pid);
        printf("  Level: %d\n", info.level);
        printf("  Ticks per level: %d %d %d %d\n", info.ticks[0], info.ticks[1], info.ticks[2], info.ticks[3]);
        printf("  Times scheduled: %d\n", info.times_scheduled);
        printf("  Total syscalls: %d\n", info.total_syscalls);
    }
    else
    {
        printf("Failed to get MLFQ info\n");
    }

    exit(0);
}