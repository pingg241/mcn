/* 文本控制台（UDP 6000，宿主版还可以直接在命令行窗口里敲）：一行一条命令，回复以 "OK" 或 "ERR ..." 行结束
 * （help 查看命令表） */
#ifndef CONSOLE_H
#define CONSOLE_H

#include <stddef.h>
#include <stdint.h>

typedef void (*console_out_fn)(const char *s, size_t n);

void console_init(void);
void console_rx(uint32_t ip, uint16_t port, const uint8_t *data, size_t len);
/* 本机终端的一行命令，回复（包括之后才到的 reg / wave 结果）交给 out */
void console_line(const char *line, console_out_fn out);
void console_poll(uint32_t now);
/* 日志环形缓冲（plat_log 的实现应调用），log 命令读出 */
void console_log(const char *s);

#endif
