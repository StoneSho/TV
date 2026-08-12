#include "Anilove.h"
#include "config.h"
#include "smimg/sm1.h"
#include "smimg/sm2.h"
#include "smimg/sm3.h"
#include "smimg/sm4.h"
#include "smimg/sm5.h"
#include "smimg/sm6.h"
#include "smimg/sm7.h"

int Anilove_key = -1;         //初始化图标显示帧数
int sm_ky = 0;                //初始化动画，一共六张
uint32_t count = 0;           //每张动画显示计数

void loveImg(const uint8_t **Animate_value, uint32_t *Animate_size, bool *swith_animate)
{

    const uint8_t **all[] = {sm7};
    const uint32_t all_length[] = {8};
    const uint32_t *all_size[] = {sm7_size};

    Anilove_key++;
    *swith_animate = false; //默认没有切换
    *Animate_value = &*all[sm_ky][Anilove_key];
    *Animate_size = all_size[sm_ky][Anilove_key];
    if (Anilove_key >= all_length[sm_ky] - 1) // >=24
    {
        Anilove_key = -1;
        count++;
    }

    if (count >= ANIMATE_CHANGE_FREQUENTCE)
    {
        sm_ky++;               //切换动画
        *swith_animate = true; //切换动画标志
        count = 0;
        Anilove_key = -1;
        if (sm_ky >= sizeof(all_length) / sizeof(all_length[0]) - 1)
        {
            sm_ky = 0;
        }
    }
}

void loveImgAll(const uint8_t **Animate_value, uint32_t *Animate_size)
{
    Anilove_key++;

    *Animate_value = sm3[Anilove_key];
    *Animate_size = sm3_size[Anilove_key];
    if (Anilove_key >= (sizeof(sm3) / sizeof(sm3[0]) - 1)) // >=24
        Anilove_key = -1;
}
