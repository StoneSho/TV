#ifndef ANIMATE_h
#define ANIMATE_h

typedef unsigned char uint8_t;
typedef unsigned int uint32_t;

// 返回 true 表示当前动画已经绘制完一整轮。
bool imgAnim(const uint8_t **Animate_value, uint32_t *Animate_size, int Animate_Choice);

#endif
