
#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>

int main()
{
    int cpu_number = syscall(452);
    printf("CPUs : %d\n", cpu_number);
}
