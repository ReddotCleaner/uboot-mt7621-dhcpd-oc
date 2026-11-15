// SPDX-License-Identifier:	GPL-2.0+
/*
 * Copyright (C) 2018 MediaTek Incorporation. All Rights Reserved.
 *
 * Author: Weijie Gao <weijie.gao@mediatek.com>
 */

#include <common.h>
#include <asm/io.h>
#include <asm/addrspace.h>
#include <asm/types.h>
#include <mach/mt7621_regs.h>

DECLARE_GLOBAL_DATA_PTR;

extern u32 mempll_get_clock(void);

int get_serial_clock(void)
{
	return 50000000;
}

void mt7621_get_clocks(u32 *pcpu_clk, u32 *pbus_clk, u32 *pddr_clk, u32 *pxtal_clk)
{
	void __iomem *sys_base, __iomem *dramc_base;
	u32 bs, xtal_sel, clkcfg0, cur_clk, mempll, dividx, fb;
	u32 xtal_clk, xtal_div, ffiv, ffrac, cpu_clk, ddr_clk;
	const static u32 xtal_div_tbl[] = {0, 1, 2, 2};

	sys_base = (void __iomem *) CKSEG1ADDR(MT7621_SYSCTL_BASE);
	dramc_base = (void __iomem *) CKSEG1ADDR(MT7621_DRAMC_BASE);

	bs = readl(sys_base + MT7621_SYS_SYSCFG0_REG);
	clkcfg0 = readl(sys_base + MT7621_SYS_CLKCFG0_REG);
	cur_clk = readl(sys_base + MT7621_SYS_CUR_CLK_STS_REG);

	xtal_sel = REG_GET_VAL(XTAL_MODE_SEL, bs);

	if (xtal_sel <= 2)
		xtal_clk = 20 * 1000 * 1000;
	else if (xtal_sel <= 5)
		xtal_clk = 40 * 1000 * 1000;
	else
		xtal_clk = 25 * 1000 * 1000;

	switch (REG_GET_VAL(CPU_CLK_SEL, clkcfg0)) {
	case 0:
		cpu_clk = 500 * 1000 * 1000;
		break;
	case 1:
		mempll = readl(dramc_base + MT7621_DRAMC_MEMPLL18_REG);
		dividx = REG_GET_VAL(RG_MEPL_PREDIV, mempll);
		fb = REG_GET_VAL(RG_MEPL_FBDIV, mempll);
		xtal_div = 1 << xtal_div_tbl[dividx];
		cpu_clk = (fb + 1) * xtal_clk / xtal_div;
		break;
	default:
		cpu_clk = xtal_clk;
	}

	ffiv = REG_GET_VAL(CUR_CPU_FDIV, cur_clk);
	ffrac = REG_GET_VAL(CUR_CPU_FFRAC, cur_clk);

	/*
	 * CUR_CPU_FDIV and CUR_CPU_FFRAC are live hardware status, not
	 * configuration: a clock block that has not come up yet reads 0 for
	 * both.  ffiv == 0 is an immediate divide by zero, and ffrac == 0
	 * drives cpu_clk to 0, which makes get_tbclk() return 0 and turns
	 * every later do_div() in lib/time.c into a divide by zero too.  On
	 * MIPS that aborts the boot with a teq exception, so treat an
	 * unusable status as a unit ratio and keep the reference rate.
	 */
	if (!ffiv || !ffrac) {
		ffiv = 1;
		ffrac = 1;
	}

	cpu_clk = cpu_clk / ffiv * ffrac;

	mempll = readl(dramc_base + MT7621_DRAMC_MEMPLL6_REG);
	dividx = REG_GET_VAL(RG_MEPL_PREDIV, mempll);
	fb = REG_GET_VAL(RG_MEPL_FBDIV, mempll);
	xtal_div = 1 << xtal_div_tbl[dividx];
	ddr_clk = fb * xtal_clk / xtal_div;

	bs = readl(dramc_base + MT7621_DRAMC_MEMPLL1_REG);
	if (REG_GET_VAL(RG_MEPL_DIV2_SEL, bs) == 0)
		ddr_clk *= 2;

	if (pcpu_clk)
		*pcpu_clk = cpu_clk;

	if (pbus_clk)
		*pbus_clk = cpu_clk / 4;

	if (pddr_clk)
		*pddr_clk = ddr_clk;

	if (pxtal_clk)
		*pxtal_clk = xtal_clk;
}

int get_clocks(void)
{
	mt7621_get_clocks((u32 *)&gd->cpu_clk, (u32 *)&gd->bus_clk,
			  (u32 *)&gd->mem_clk, (u32 *)&gd->arch.xtal_clk);

	return 0;
}

ulong get_cpu_freq(ulong dummy)
{
	if (!gd->cpu_clk)
		get_clocks();
	return gd->cpu_clk;
}

ulong get_bus_freq(ulong dummy)
{
	if (!gd->bus_clk)
		get_clocks();
	return gd->bus_clk;
}

ulong get_ddr_freq(ulong dummy)
{
	if (!gd->mem_clk)
		get_clocks();
	return gd->mem_clk;
}

ulong notrace get_tbclk(void)
{
	return get_cpu_freq(0) / 2;
}

/*
 * Short settle delay used while the CPU is parked on the reference PLL.
 * The counter is volatile so the compiler cannot drop the loop: the vendor
 * sequence leaves about 4096 iterations between each clock switch and the
 * PLL register access, and without a real delay the switch has not settled.
 */
static void mt7621_pll_settle(void)
{
	volatile int i;

	for (i = 0; i < 4096; i++)
		;
}

/*
 * Retune the MT7621 CPU clock at run time.
 *
 * MT7621 derives the CPU clock from MEMPLL (DRAMC MEMPLL18), the same PLL
 * domain the DRAM controller uses, so it cannot be reprogrammed while the
 * CPU is clocked from it.  The sequence used here is the one validated on
 * the AC2100 build of this bootloader:
 *
 *   1. clear CPU_CLK_SEL so the CPU runs from the fixed 500 MHz reference,
 *   2. touch SYSCTL 0x410 (reference clock gate used by the vendor DRAM init),
 *   3. rewrite RG_MEPL_PREDIV / RG_MEPL_FBDIV in MEMPLL18, keeping every
 *      other bit (enable, VOD, loop filter) untouched,
 *   4. set CPU_CLK_SEL again so the CPU runs from the retuned MEMPLL,
 *   5. verify the result against get_cpu_freq().
 *
 * Only frequencies that map to an integer FBDIV for the board's crystal are
 * accepted; anything else returns -EINVAL before a register is touched.
 */
int mt7621_set_cpu_freq(u32 mhz)
{
	void __iomem *sys_base = (void __iomem *)CKSEG1ADDR(MT7621_SYSCTL_BASE);
	void __iomem *dramc_base = (void __iomem *)CKSEG1ADDR(MT7621_DRAMC_BASE);
	const static u8 xtal_div_tbl[] = { 0, 1, 2, 2 };
	u32 xtal_hz = 0, xtal_mhz, prediv, fbdiv, num, v, clk;

	mt7621_get_clocks(NULL, NULL, NULL, &xtal_hz);
	xtal_mhz = xtal_hz / 1000000;
	if (!xtal_mhz || xtal_mhz > 100)
		return -EINVAL;

	v = readl(dramc_base + MT7621_DRAMC_MEMPLL18_REG);
	prediv = REG_GET_VAL(RG_MEPL_PREDIV, v);

	num = mhz << xtal_div_tbl[prediv];
	if (num % xtal_mhz)
		return -EINVAL;

	fbdiv = num / xtal_mhz - 1;
	if (fbdiv >= (RG_MEPL_FBDIV_M + 1))
		return -EINVAL;

	/* Park the CPU on the fixed reference PLL while MEMPLL is retuned */
	clk = readl(sys_base + MT7621_SYS_CLKCFG0_REG);
	writel(clk & ~(0x3u << 30), sys_base + MT7621_SYS_CLKCFG0_REG);
	mt7621_pll_settle();

	writel(0x101, sys_base + 0x410);

	v = readl(dramc_base + MT7621_DRAMC_MEMPLL18_REG);
	v &= ~((RG_MEPL_FBDIV_M << RG_MEPL_FBDIV_S) |
	       (RG_MEPL_PREDIV_M << RG_MEPL_PREDIV_S) |
	       (1u << RG_MEPL_LF_S));
	v |= (fbdiv << RG_MEPL_FBDIV_S) | (prediv << RG_MEPL_PREDIV_S);
	writel(v, dramc_base + MT7621_DRAMC_MEMPLL18_REG);
	mt7621_pll_settle();

	clk = readl(sys_base + MT7621_SYS_CLKCFG0_REG);
	writel((clk & ~(0x3u << 30)) | (1u << 30),
	       sys_base + MT7621_SYS_CLKCFG0_REG);
	mt7621_pll_settle();

	gd->cpu_clk = 0;
	clk = (u32)get_cpu_freq(0);

	return (clk / 1000000 == mhz) ? 0 : -EIO;
}