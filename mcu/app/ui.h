/* LCD 界面：能谱 / 波形 / 统计 三页。KEY 短按切页；长按在波形页换信号源，其余页清谱 */
#ifndef UI_H
#define UI_H

#include <stdint.h>

enum { UI_SPECTRUM = 0, UI_WAVE, UI_STATS, UI_NSCREENS };

void ui_init(void);
void ui_poll(uint32_t now);
void ui_set_screen(int s);
int  ui_screen(void);
void ui_draw(void);          /* 立即重画整屏到帧缓冲（不 flush） */

#endif
