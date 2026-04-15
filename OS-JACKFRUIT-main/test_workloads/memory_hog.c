#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main() {
    printf("Memory Hog started. Allocating memory...\n");
    while (1) {
        // Allocate 1MB at a time
        void *p = malloc(1024 * 1024);
        if (p) {
            memset(p, 0, 1024 * 1024);
            printf("Allocated 1MB...\n");
        }
        sleep(1); 
    }
    return 0;
}
