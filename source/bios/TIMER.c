#include "3ds.h"

#include "fix.h"

/*vu64    TickCounter;

ITCM_CODE void CountUpTick (void)
{
    TickCounter++; // will be incremented ~ 1000 times/sec
}

ITCM_CODE fix timer_get_fixed_seconds ()
{
    return (fix) (TickCounter / 1000 * F1_0) + ((TickCounter % 1000) * F1_0 + 500) / 1000;
}

void timer_init ()
{
    TickCounter = 0;
    timerStop(0);
	timerStart(0, ClockDivider_1, TIMER_FREQ(1000), CountUpTick);
}

void delay(int d_time)
{
	fix t, total;

	t = timer_get_fixed_seconds();
	total = (F1_0 * d_time) / 1000;
	while (timer_get_fixed_seconds() - t < total) ;
}
*/

TickCounter tickCounter;

ITCM_CODE void CountUpTick(void)
{
    osTickCounterUpdate(&tickCounter);
}

ITCM_CODE fix timer_get_fixed_seconds ()
{
    return (fix) (osTickCounterRead(&tickCounter));
}

void timer_init ()
{
	osTickCounterStart(&tickCounter);
}

void delay(int d_time)
{
    fix t, total;

    t = timer_get_fixed_seconds();
    total = (F1_0 * d_time) / 1000;
    while (timer_get_fixed_seconds() - t < total) ;
}
