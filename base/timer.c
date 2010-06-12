#include <g-bios.h>

#ifdef CONFIG_IRQ_SUPPORT

#define DEFAULT_LOOPS_PERJIFFIES 1 << 8

static volatile u32 g_tick_count = 1;

static volatile u32 loops_perjiffies = DEFAULT_LOOPS_PERJIFFIES;

static volatile u32 loops_perusec = DEFAULT_LOOPS_PERJIFFIES; //fixme

void inc_tick(void)
{
	g_tick_count++;
}


u32 get_tick(void)
{
	return g_tick_count;
}


void mdelay(u32 n)
{
	volatile u32 curr_tick = get_tick();


	// yes, we'd write the loop in this way :P
	while (1)
	{
		if (get_tick() >= curr_tick + n)
			return;
	}
}


extern void __udelay(u32 n);

void calibrate_delay(u32 ticks_persecond)
{
	volatile u32 cur_ticks;

	printf("Default	loops for perjiffies is %d\n", loops_perjiffies);

	while (1)
	{
		cur_ticks = g_tick_count;
		while (cur_ticks == g_tick_count);

		cur_ticks = g_tick_count;

		__udelay(loops_perjiffies);

		cur_ticks = g_tick_count - cur_ticks;
		if (cur_ticks)
		{
			break;
		}

		loops_perjiffies <<= 1;
	}

	loops_perjiffies >>= 1;

	while (1)
	{
		cur_ticks = g_tick_count;
		while (cur_ticks == g_tick_count);

		cur_ticks = g_tick_count;

		__udelay(loops_perjiffies);

		cur_ticks = g_tick_count - cur_ticks;
		if (cur_ticks)
		{
			break;
		}

		loops_perjiffies += 1;
	}

	printf("Real	loops for perjiffies is %d\n", loops_perjiffies);
#define CONFIG_DEBUG
#ifdef CONFIG_DEBUG
	printf("----%d \n", g_tick_count);
	__udelay(loops_perjiffies * 1000 * 10);
	printf("----%d \n", g_tick_count);
#endif

	loops_perusec = (loops_perjiffies * ticks_persecond) / 1000000; //fixme
}

void udelay(u32 n)
{
	__udelay(loops_perusec * n);
}

#else
void udelay(u32 n)
{
	volatile u32 m = n * (HCLK_RATE >> 20) >> 6;

	while (m-- > 0);
}

void mdelay(u32 n)
{
	udelay(1000 * n);
}

#endif
