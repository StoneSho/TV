#ifndef MISSU_H
#define MISSU_H

#include <TFT_eSPI.h>
#include <SPI.h>
#include <TJpg_Decoder.h>

#include <misspage/img/img_miu.h>

#define RECEIVER 0
#define SENDER 1

extern TFT_eSPI tft;

//显示想你界面
void show_missu_page(String word, uint16_t word_color);

#endif