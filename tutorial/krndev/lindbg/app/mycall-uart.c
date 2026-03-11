#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/kernel.h>

#define __NR_serial_tx 453

int main() {
    const char *buf = "Hello, UART (N-INT)!\n";
    int port = 0;
    syscall(__NR_serial_tx, port, buf);
    return 0;
}