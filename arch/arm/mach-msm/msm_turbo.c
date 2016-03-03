/* arch/arm/mach-msm/msm_turbo.c
 *
 * MSM architecture cpufreq turbo boost driver
 *
 * Copyright (c) 2012-2014, Paul Reioux. All rights reserved.
 * Copyright (c) 2016, Yusuke Fukutsuka. All rights reserved.
 * Author: Paul Reioux <reioux@gmail.com>
 * Author: Yusuke Fukutsuka <donsuke.f@gmail.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/cpumask.h>

#ifdef CONFIG_TURBO_BOOST
#define STOCK_CPU_MAX_SPEED    2265600
#endif

static bool msm_turbo_enabled = false;
module_param_named(enabled, msm_turbo_enabled, bool, 0664);
static int core_threshold = 2;
module_param_named(core_threshold, core_threshold, int, 0664);
static int non_turbo_freq = STOCK_CPU_MAX_SPEED;
module_param_named(non_turbo_freq, non_turbo_freq, int, 0664);

int msm_turbo(int cpufreq)
{
	if (msm_turbo_enabled && num_online_cpus() > core_threshold) {
		if (cpufreq > non_turbo_freq)
			cpufreq = non_turbo_freq;
        }
	return cpufreq;
}

static int msm_turbo_boost_init(void)
{
	return 0;
}

static void msm_turbo_boost_exit(void)
{

}

module_init(msm_turbo_boost_init);
module_exit(msm_turbo_boost_exit);

MODULE_LICENSE("GPL V2");
MODULE_AUTHOR("Paul Reioux <reioux@gmail.com>");
MODULE_AUTHOR("Yusuke Fukutsuka <donsuke.f@gmail.com>");
MODULE_DESCRIPTION("MSM turbo boost module");

