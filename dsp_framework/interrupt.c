#include "interrupt.h"

uint32_t elapsed = 0;

void ISR()
{
    uint32_t begin = xthal_get_ccount();





    elapsed = xthal_get_ccount() - begin;
}
