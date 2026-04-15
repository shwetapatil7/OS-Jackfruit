#include <stdio.h>

int main() {
    printf("CPU Hog started. Spinning...\n");
    unsigned long long count = 0;
    while (1) {
        count++; // Just keep the CPU busy
    }
    return 0;
}
