#ifndef ANILOVE_h
#define ANILOVE_h

typedef unsigned char uint8_t;
typedef unsigned int uint32_t;

void loveImg(const uint8_t **Animate_value, uint32_t *Animate_size,bool *swith_animate); //动画函数
uint32_t arr_length(const uint8_t *arr[]);

#endif