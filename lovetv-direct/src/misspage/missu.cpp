#include "missu.h"
#include "font/LoveTV_UI_18.h"


void show_missu_page(String word, uint16_t word_color)
{
    TFT_eSprite miu = TFT_eSprite(&tft);
    uint16_t pinkColor = tft.color565(255, 174, 201);
    uint16_t zongseColor = tft.color565(128, 64, 64);
    //显示想你图片
    TJpgDec.drawJpg(70, 10, misu_img, sizeof(misu_img));
    
    miu.setColorDepth(16);
    miu.loadFont(LoveTV_UI_18);
   
    if (word != "")
    {
        miu.createSprite(240, 20);
        miu.fillSprite(pinkColor);
        miu.setTextWrap(false);
        miu.setTextDatum(CC_DATUM);
        miu.setTextColor(word_color);
        miu.drawString(word, 120, 11);
        miu.pushSprite(0, 150);
        miu.deleteSprite();
    }

    miu.setColorDepth(16);
    miu.createSprite(240, 20);
    miu.fillSprite(pinkColor);
    miu.setTextWrap(false);
    miu.setTextDatum(CC_DATUM);
    miu.setTextColor(zongseColor);
    miu.drawString("（单击按钮退出！）", 120, 11);
    miu.pushSprite(0, 210);
    miu.deleteSprite();

    miu.unloadFont();
}
