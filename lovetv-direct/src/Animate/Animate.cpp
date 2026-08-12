#include "Animate.h"
#include "config.h"
#include "img/astronaut.h"
#include "img/hutao.h"

namespace {
uint8_t animateKey = 0;
int activeChoice = 0;
}

bool imgAnim(const uint8_t **Animate_value, uint32_t *Animate_size, int Animate_Choice) {
  if (Animate_Choice != 2 && Animate_Choice != 3) {
    Animate_Choice = 2;
  }

  if (Animate_Choice != activeChoice) {
    activeChoice = Animate_Choice;
    animateKey = 0;
  }

  const uint8_t frameCount = (Animate_Choice == 2) ? 10 : 32;
  if (Animate_Choice == 2) {
    *Animate_value = astronaut[animateKey];
    *Animate_size = astronaut_size[animateKey];
  } else {
    *Animate_value = hutao[animateKey];
    *Animate_size = hutao_size[animateKey];
  }

  const bool finished = animateKey == frameCount - 1;
  animateKey = finished ? 0 : animateKey + 1;
  return finished;
}
