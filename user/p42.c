// // user/p42.c  —  FCFS vs SSTF disk scheduling test
// //
// // Compile-time requirements:
// //   kernel/proc.h  : struct vmstats { ... struct disc_stats disk; }  (by value)
// //   user/user.h    : struct disc_stats { uint64 reads,writes,total_latency; }
// //                    struct vmstats    { ... struct disc_stats disk; }  (by value)
// //   kernel/proc.c  : kgetvmstats fills info->disk.{reads,writes,total_latency}
// //                    from p->disk_reads / p->disk_writes / p->total_disk_latency
// //
// // DO NOT #include "kernel/proc.h" in user programs — it pulls in kernel-only
// // types (pagetable_t, spinlock, NCPU, NOFILE, MAX_PSYC_PAGES) that don't exist
// // in userspace and causes redefinition errors for vmstats and mlfqinfo.

// #include "kernel/types.h"
// #include "kernel/stat.h"
// #include "user/user.h"

// #define NPAGES 160

// // Run one round: set policy, stress the VM/disk, print stats.
// // Returns total_latency so main() can compare the two policies.
// uint64 run_test(int policy, const char *name)
// {
//     setdisksched(policy);

//     // --- Allocate NPAGES pages lazily (sbrk does not touch disk yet) ---
//     char *base = sbrk(NPAGES * 4096);
//     if (base == (char *)-1) {
//         printf("sbrk failed\n");
//         exit(1);
//     }

//     // --- Write in a scattered pattern to maximise head movement ---
//     // Pass 1: even pages  (0, 8192, 16384, ...)
//     // Pass 2: odd pages   (4096, 12288, 20480, ...)
//     // This interleaving forces large seeks under FCFS; SSTF should
//     // reorder to serve nearby blocks first, reducing total_latency.
//     for (int i = 0; i < NPAGES; i += 2)
//         base[i * 4096] = (char)i;
//     for (int i = 1; i < NPAGES; i += 2)
//         base[i * 4096] = (char)i;

//     // --- Read back everything (forces any pending swap-ins) ---
//     int chk = 0;
//     for (int i = 0; i < NPAGES; i++)
//         chk += (unsigned char)base[i * 4096];

//     // --- Collect stats ---
//     // getvmstats now copies disc_stats by value so disk.reads etc. are valid.
//     struct vmstats st;
//     if (getvmstats(getpid(), &st) < 0) {
//         printf("getvmstats failed\n");
//         exit(1);
//     }

//     printf("[%-4s]  page_faults=%-4d  evicted=%-4d  swapped_out=%-4d  swapped_in=%-4d\n",
//            name,
//            st.page_faults,
//            st.pages_evicted,
//            st.pages_swapped_out,
//            st.pages_swapped_in);

//     // disk fields are uint64 — use %lu
//     printf("         disk_reads=%-6lu  disk_writes=%-6lu  total_latency=%-10lu  checksum=%d\n",
//            st.disk.reads,
//            st.disk.writes,
//            st.disk.total_latency,
//            chk);

//     // Release memory before next round so physical frames are freed
//     sbrk(-(NPAGES * 4096));

//     return st.disk.total_latency;
// }

// int main(void)
// {
//     printf("=== Disk scheduling comparison (FCFS vs SSTF) ===\n\n");

//     uint64 lat_fcfs = run_test(0, "FCFS");
//     printf("\n");
//     uint64 lat_sstf = run_test(1, "SSTF");

//     printf("\n--- Result ---\n");
//     printf("FCFS total_latency : %lu\n", lat_fcfs);
//     printf("SSTF total_latency : %lu\n", lat_sstf);

//     if (lat_sstf <= lat_fcfs)
//         printf("PASS: SSTF <= FCFS  (SSTF saved %lu latency units)\n",
//                lat_fcfs - lat_sstf);
//     else
//         printf("FAIL: SSTF (%lu) > FCFS (%lu) — check sched_pick_target()\n",
//                lat_sstf, lat_fcfs);

//     exit(0);
// }

// user/p42.c  —  FCFS vs SSTF disk scheduling test
//
// Three bugs fixed vs the previous version:
//
//  1. Printf width specifiers (%-4d, %-6lu) are not supported by xv6's
//     minimal printf — they printed literally.  Use plain %d / %lu.
//
//  2. getvmstats returns *cumulative* stats since process start.  The SSTF
//     run was including all latency from the FCFS run.  Fixed by snapshotting
//     stats before each run and reporting the delta.
//
//  3. The disk queue is always size 1 when sched_pick_target() is called,
//     because enqueue→pick→dequeue happens atomically under the vdisk_lock.
//     SSTF can only reorder when multiple requests are queued simultaneously.
//     Fixed by forking N_CHILDREN processes per round so their I/O requests
//     race into the queue at the same time.  Each child reports its own delta
//     latency back to the parent through a pipe; the parent sums them.

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define NPAGES     200          // pages per child  (less than before — N children × NPAGES total)
#define N_CHILDREN  3     // concurrent child processes per round

// Snapshot of the stats we care about, taken before and after a run.
struct snap {
    uint64 disk_reads;
    uint64 disk_writes;
    uint64 total_latency;
    int    page_faults;
    int    swapped_out;
    int    swapped_in;
};

static void take_snap(struct snap *s)
{
    struct vmstats st;
    getvmstats(getpid(), &st);
    s->disk_reads    = st.disk.reads;
    s->disk_writes   = st.disk.writes;
    s->total_latency = st.disk.total_latency;
    s->page_faults   = st.page_faults;
    s->swapped_out   = st.pages_swapped_out;
    s->swapped_in    = st.pages_swapped_in;
}

// One child: allocate NPAGES, touch them in a scattered pattern, read back,
// write delta stats to the pipe, then exit.
static void child_work(int write_fd, int child_id)
{
    struct snap before, after;
    take_snap(&before);

    char *base = sbrk(NPAGES * 4096);
    if (base == (char *)-1) {
        printf("child %d: sbrk failed\n", child_id);
        exit(1);
    }

    // Scattered write: even pages first, then odd.
    // Adjacent children operate on different virtual regions but their swap
    // blocks are spread across the disk, so concurrent children generate
    // requests at widely separated block numbers — exactly the workload
    // where SSTF beats FCFS.
    for (int i = 0; i < NPAGES; i += 2)
        base[i * 4096] = (char)(child_id * 10 + i);
    for (int i = 1; i < NPAGES; i += 2)
        base[i * 4096] = (char)(child_id * 10 + i);

    // Read back (forces swap-ins for any evicted pages).
    int chk = 0;
    for (int i = 0; i < NPAGES; i++)
        chk += (unsigned char)base[i * 4096];

    sbrk(-(NPAGES * 4096));

    take_snap(&after);

    // Report deltas to parent via the pipe.
    // Pack as: reads  writes  latency  faults  swapped_out  swapped_in  chk
    uint64 buf[7];
    buf[0] = after.disk_reads    - before.disk_reads;
    buf[1] = after.disk_writes   - before.disk_writes;
    buf[2] = after.total_latency - before.total_latency;
    buf[3] = (uint64)(after.page_faults  - before.page_faults);
    buf[4] = (uint64)(after.swapped_out  - before.swapped_out);
    buf[5] = (uint64)(after.swapped_in   - before.swapped_in);
    buf[6] = (uint64)chk;
    write(write_fd, buf, sizeof(buf));
    close(write_fd);
    exit(0);
}

// Run one round: fork N_CHILDREN children, collect their delta stats,
// print a summary and return the total latency across all children.
uint64 run_test(int policy, const char *name)
{
    setdisksched(policy);

    int pipefd[N_CHILDREN][2];
    for (int c = 0; c < N_CHILDREN; c++) {
        printf("[parent] forking child %d for %s test...\n", c, name);
        if (pipe(pipefd[c]) < 0) {
            printf("pipe failed\n");
            exit(1);
        }
        int pid = fork();
        if (pid < 0) {
            printf("fork failed\n");
            exit(1);
        }
        if (pid == 0) {
            // Child: close all read ends + other children's write ends,
            // then do the work.
            close(pipefd[c][0]);
            // close sibling write ends already opened
            for (int j = 0; j < c; j++) {
                close(pipefd[j][0]);
                close(pipefd[j][1]);
            }
            child_work(pipefd[c][1], c);
            // child_work calls exit(), never returns
        }
        // Parent: close the write end for this child.
        close(pipefd[c][1]);
    }

    // Collect results from all children.
    uint64 total_reads = 0, total_writes = 0, total_latency = 0;
    uint64 total_faults = 0, total_swout = 0, total_swin = 0;
    uint64 total_chk = 0;

    for (int c = 0; c < N_CHILDREN; c++) {
        uint64 buf[7];
        int n = read(pipefd[c][0], buf, sizeof(buf));
        close(pipefd[c][0]);
        if (n != sizeof(buf)) {
            printf("child %d pipe read failed (got %d)\n", c, n);
            continue;
        }
        total_reads   += buf[0];
        total_writes  += buf[1];
        total_latency += buf[2];
        total_faults  += buf[3];
        total_swout   += buf[4];
        total_swin    += buf[5];
        total_chk     += buf[6];
    
        wait(0);
    }
    printf("[%s] faults=%lu swapped_out=%lu swapped_in=%lu\n",
           name, total_faults, total_swout, total_swin);
    printf("     reads=%lu writes=%lu latency=%lu checksum=%lu\n",
           total_reads, total_writes, total_latency, total_chk);

    return total_latency;
}

int main(void)
{
    printf("=== Disk scheduling: FCFS vs SSTF (%d concurrent children, %d pages each) ===\n\n",
           N_CHILDREN, NPAGES);

    uint64 lat_fcfs = run_test(0, "FCFS");
    printf("\n");
    uint64 lat_sstf = run_test(1, "SSTF");

    printf("\n--- Result ---\n");
    printf("FCFS latency : %lu\n", lat_fcfs);
    printf("SSTF latency : %lu\n", lat_sstf);

    if (lat_sstf <= lat_fcfs)
        printf("PASS: SSTF <= FCFS (saved %lu latency units)\n",
               lat_fcfs - lat_sstf);
    else
        printf("FAIL: SSTF (%lu) > FCFS (%lu)\n", lat_sstf, lat_fcfs);

    exit(0);
}