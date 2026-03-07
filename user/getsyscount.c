#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main(void) {
    int count;
    // int pid;
    
    //normal case
    count = getsyscount();
    printf("First call: %d\n", count);
    printf("Making second call.\n");
    int count2 = getsyscount();
    printf("Second call: %d\n", count2);
    
}