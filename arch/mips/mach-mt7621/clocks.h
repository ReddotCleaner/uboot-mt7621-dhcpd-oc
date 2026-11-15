/* SPDX-License-Identifier:	GPL-2.0+ */
/*
 * Copyright (C) 2018 MediaTek Incorporation. All Rights Reserved.
 *
 * Author: Weijie Gao <weijie.gao@mediatek.com>
 */

#ifndef _MACH_MT7621_CLOCKS_H_
#define _MACH_MT7621_CLOCKS_H_

extern void mt7621_get_clocks(u32 *pcpu_clk, u32 *pbus_clk, u32 *pddr_clk, u32 *pxtal_clk);
extern ulong get_cpu_freq(ulong dummy);

/*
 * Retune the CPU clock at run time.  The CPU is moved off the memory PLL,
 * MEMPLL is reprogrammed, and the CPU is switched back, so the DRAM clock is
 * never disturbed.  Returns 0 when the new rate has been verified.
 */
extern int mt7621_set_cpu_freq(u32 mhz);

#endif /* _MACH_MT7621_CLOCKS_H_ */
