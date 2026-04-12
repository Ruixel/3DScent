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

// osGetTime() returns monotonic milliseconds directly from the 3DS system clock,
// requiring no interrupt handler or manual update calls.
// Convert to fix: F1_0 = 0x10000 = 1 second, so ms * 65536 / 1000.
// The result wraps in int32 for large times but elapsed differences remain correct.
ITCM_CODE fix timer_get_fixed_seconds ()
{
    return (fix) ((u64)osGetTime() * 0x10000 / 1000);
}

void timer_init ()
{
    // nothing to initialise — osGetTime() reads the system clock directly
}

void delay(int d_time)
{
    fix t, total;

    t = timer_get_fixed_seconds();
    total = (F1_0 * d_time) / 1000;
    while (timer_get_fixed_seconds() - t < total) ;
}
