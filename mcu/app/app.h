/* MCU 应用主体：平台层只需调用 app_init / app_poll / app_udp_rx */
#ifndef APP_H
#define APP_H

#include <stddef.h>
#include <stdint.h>

void app_init(void);
void app_poll(void);
/* 平台收到 UDP 数据报（local_port 为本机端口 5000/5001/6000） */
void app_udp_rx(uint16_t local_port, uint32_t src_ip, uint16_t src_port, const uint8_t *data, size_t len);

void   app_start(double preset_live_s);   /* preset 为 0 表示不限时 */
void   app_stop(void);
void   app_clear(void);                    /* 清 FPGA 计数器与能谱 */
double app_preset(void);
/* 配置项被修改后调用（key 为 "*" 表示全部），按需重新下发 FPGA、清谱或重算峰 */
void   app_setting_changed(const char *key);
void   app_reanalyze(void);

#endif
