/* newlib 需要的最小系统调用：堆在 AXI SRAM（.bss 之后到末尾），stdout 写调试串口 */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>

#include "board.h"

extern UART_HandleTypeDef huart_log;
extern uint8_t _heap_start, _heap_end;

void *_sbrk(ptrdiff_t incr)
{
    static uint8_t *brk = &_heap_start;
    if (brk + incr > &_heap_end) {
        errno = ENOMEM;
        return (void *)-1;
    }
    uint8_t *prev = brk;
    brk += incr;
    return prev;
}

int _write(int fd, const char *buf, int len)
{
    (void)fd;
    HAL_UART_Transmit(&huart_log, (uint8_t *)buf, (uint16_t)len, 100);
    return len;
}

int _read(int fd, char *buf, int len) { (void)fd; (void)buf; (void)len; return 0; }
int _close(int fd) { (void)fd; return -1; }
int _lseek(int fd, int off, int whence) { (void)fd; (void)off; (void)whence; return 0; }
int _isatty(int fd) { (void)fd; return 1; }
int _fstat(int fd, struct stat *st) { (void)fd; st->st_mode = S_IFCHR; return 0; }
int _getpid(void) { return 1; }
int _kill(int pid, int sig) { (void)pid; (void)sig; errno = EINVAL; return -1; }
void _exit(int status) { (void)status; for (;;) {} }
