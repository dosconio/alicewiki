#include <linux/serial_8250.h>
"/usr/src/linux-6.5/drivers/tty/serial/8250/8250.h"

/*
*/
SYSCALL_DEFINE0(get_cpu_number)
{
	return num_present_cpus();
}

/**
 * sys_my_uart_send - uart out by syscall but print-interrupt
 *
 * Dosconio Revive hers 20241030
 */
SYSCALL_DEFINE2(my_uart_send, int, uart_port, const char __user*, buf) {
	struct uart_8250_port* up;
	struct uart_port* port;
	struct circ_buf* xmit;
	char kernel_buf[64];//{} use kmalloc and kfree
	size_t len = sizeof(kernel_buf);
	up = serial8250_get_port(uart_port);
	port = &up->port;
	xmit = &port->state->xmit;
	if (copy_from_user(kernel_buf, buf, len)) return -EFAULT;
	len = strlen(kernel_buf);// min(len, xmit->size - uart_circ_chars_pending(xmit));
	kernel_buf[64 - 1] = 0;
	up->tx_loadsz = len;
	xmit->head += len;
	for (int i = 0; i < len; i++) {
		xmit->buf[xmit->tail + i] = kernel_buf[i];
	}
	serial8250_tx_chars(up);
	return 0;
}