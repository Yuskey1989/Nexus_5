/*
 * drivers/cpufreq/cpufreq_interest.c
 *
 * Copyright (C) 2010 Google, Inc.
 * Copyright (C) 2015 Yusuke Fukutsuka
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
 * Author: Mike Chan (mike@android.com)
 * Author: Yusuke Fukutsuka (donsuke.f@gmail.com)
 *
 */

#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/cpufreq.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/rwsem.h>
#include <linux/sched.h>
#include <linux/tick.h>
#include <linux/time.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/kernel_stat.h>
#include <asm/cputime.h>
#include <linux/input.h>

#ifndef CPUFREQ_RELATION_C
#define CPUFREQ_RELATION_C CPUFREQ_RELATION_L
#endif

static int active_count;

struct cpufreq_interactive_cpuinfo {
	struct timer_list cpu_timer;
	struct timer_list cpu_slack_timer;
	spinlock_t load_lock; /* protects the next 4 fields */
	u64 time_in_idle;
	u64 time_in_idle_timestamp;
	u64 cputime_speedadj;
	u64 cputime_speedadj_timestamp;
	u64 last_evaluated_jiffy;
	struct cpufreq_policy *policy;
	struct cpufreq_frequency_table *freq_table;
	spinlock_t target_freq_lock; /*protects target freq */
	unsigned int target_freq;
	unsigned int floor_freq;
	unsigned int min_freq;
	u64 floor_validate_time;
	u64 hispeed_validate_time; /* cluster hispeed_validate_time */
	u64 local_hvtime; /* per-cpu hispeed_validate_time */
	u64 max_freq_hyst_start_time;
	struct rw_semaphore enable_sem;
	int governor_enabled;
	int prev_load;
};

static DEFINE_PER_CPU(struct cpufreq_interactive_cpuinfo, cpuinfo);

/* realtime thread handles frequency scaling */
static struct task_struct *speedchange_task;
static cpumask_t speedchange_cpumask;
static spinlock_t speedchange_cpumask_lock;
static struct mutex gov_lock;

/* Hi speed to bump to from lo speed when load burst (default max) */
static unsigned int hispeed_freq;

/* Boost speed when touch screen */
#define DEFAULT_INPUT_BOOST_FREQ 1500000
static unsigned int input_boost_freq = DEFAULT_INPUT_BOOST_FREQ;

#define DEFAULT_IDEAL_FREQ 1000000
static unsigned int ideal_freq = DEFAULT_IDEAL_FREQ;
/* When below the ideal_freq we always ramp up to the ideal_freq */
static bool go_up_to_ideal_freq = false;
/* When above the ideal_freq we always ramp down to the ideal_freq */
static bool go_down_to_ideal_freq = false;

/* Go to hi speed when CPU load at or above this value. */
#define DEFAULT_GO_HISPEED_LOAD 99
static unsigned long go_hispeed_load = DEFAULT_GO_HISPEED_LOAD;

/* Go to lowest speed when CPU load at or below this value. */
#define DEFAULT_GO_LOSPEED_LOAD 5
static unsigned long go_lospeed_load = DEFAULT_GO_LOSPEED_LOAD;

/* Target load.  Lower values result in higher CPU speeds. */
#define DEFAULT_TARGET_LOAD 90
static unsigned int default_target_loads[] = {DEFAULT_TARGET_LOAD};
static spinlock_t target_loads_lock;
static unsigned int *target_loads = default_target_loads;
static int ntarget_loads = ARRAY_SIZE(default_target_loads);

/*
 * The minimum amount of time to spend at a frequency before we can ramp down.
 */
#define DEFAULT_MIN_SAMPLE_TIME (80 * USEC_PER_MSEC)
static unsigned long min_sample_time = DEFAULT_MIN_SAMPLE_TIME;

/*
 * The sample rate of the timer used to increase frequency
 */
#define DEFAULT_TIMER_RATE (20 * USEC_PER_MSEC)
static unsigned long timer_rate = DEFAULT_TIMER_RATE;

/*
 * Wait this long before raising speed above hispeed, by default a single
 * timer interval.
 */
#define DEFAULT_ABOVE_HISPEED_DELAY DEFAULT_TIMER_RATE
static unsigned int default_above_hispeed_delay[] = {
	DEFAULT_ABOVE_HISPEED_DELAY };
static spinlock_t above_hispeed_delay_lock;
static unsigned int *above_hispeed_delay = default_above_hispeed_delay;
static int nabove_hispeed_delay = ARRAY_SIZE(default_above_hispeed_delay);

/* Non-zero means indefinite speed boost active */
static int boost_val;
/* Duration of a boot pulse in usecs */
static int boostpulse_duration_val = DEFAULT_MIN_SAMPLE_TIME;
/* Duration of an input boot pulse in usecs */
static int input_boostpulse_duration_val = DEFAULT_MIN_SAMPLE_TIME;
/* End time of boost pulse in ktime converted to usecs */
static u64 boostpulse_endtime;
/* End time of input boost pulse in ktime converted to usecs */
static u64 input_boostpulse_endtime;

/*
 * Max additional time to wait in idle, beyond timer_rate, at speeds above
 * minimum before wakeup to reduce speed, or -1 if unnecessary.
 */
#define DEFAULT_TIMER_SLACK (4 * DEFAULT_TIMER_RATE)
static int timer_slack_val = DEFAULT_TIMER_SLACK;

/*
 * Whether to align timer windows across all CPUs. When
 * use_sched_load is true, this flag is ignored and windows
 * will always be aligned.
 */
static bool align_windows = true;

/* Improves frequency selection for more energy */
#define POWERSAVE_BIAS_MAXLEVEL			(1000)
#define POWERSAVE_BIAS_MINLEVEL			(-1000)
static int powersave_bias;

/*
 * Stay at max freq for at least max_freq_hysteresis before dropping
 * frequency.
 */
static unsigned int max_freq_hysteresis;

static unsigned int io_is_busy;

/*
 * Go to max frequency when current frequency is above hispeed and
 * current load exceeds hispeed load continuing plus_ondemand times
 */
static unsigned int plus_ondemand;

/*
 * Freqeuncy delta when ramping up.
 * Zero disables and will calculate ramp up according to load heuristic.
 * (The load heuristic calculation is the original interactive works.)
 */
static unsigned int ramp_up_step;

/*
 * Freqeuncy delta when ramping down.
 * Zero disables and will calculate ramp down according to load heuristic.
 * (The load heuristic calculation is the original interactive works.)
 */
static unsigned int ramp_down_step;

/*
 * If the max load among other CPUs is higher than up_threshold_any_cpu_load
 * and if the highest frequency among the other CPUs is higher than
 * up_threshold_any_cpu_freq then do not let the frequency to drop below
 * sync_freq
 */
static unsigned int up_threshold_any_cpu_load = DEFAULT_TARGET_LOAD;
static unsigned int sync_freq = DEFAULT_IDEAL_FREQ;
static unsigned int up_threshold_any_cpu_freq = DEFAULT_INPUT_BOOST_FREQ;

/* Round to starting jiffy of next evaluation window */
static u64 round_to_nw_start(u64 jif)
{
	unsigned long step = usecs_to_jiffies(timer_rate);
	u64 ret;

	if (align_windows) {
		do_div(jif, step);
		ret = (jif + 1) * step;
	} else {
		ret = jiffies + usecs_to_jiffies(timer_rate);
	}

	return ret;
}

static void cpufreq_interactive_timer_resched(unsigned long cpu,
					      bool slack_only)
{
	struct cpufreq_interactive_cpuinfo *pcpu = &per_cpu(cpuinfo, cpu);
	u64 expires;
	unsigned long flags;
	u64 now = ktime_to_us(ktime_get());

	spin_lock_irqsave(&pcpu->load_lock, flags);
	expires = round_to_nw_start(pcpu->last_evaluated_jiffy);
	if (!slack_only) {
		pcpu->time_in_idle =
			get_cpu_idle_time(smp_processor_id(),
				  &pcpu->time_in_idle_timestamp, io_is_busy);
		pcpu->cputime_speedadj = 0;
		pcpu->cputime_speedadj_timestamp = pcpu->time_in_idle_timestamp;
		del_timer(&pcpu->cpu_timer);
		pcpu->cpu_timer.expires = expires;
		add_timer_on(&pcpu->cpu_timer, cpu);
	}

	if (timer_slack_val >= 0 &&
	    (pcpu->target_freq > pcpu->policy->min ||
		(pcpu->target_freq == pcpu->policy->min &&
		  (now < boostpulse_endtime ||
		   now < input_boostpulse_endtime)))) {
		expires += usecs_to_jiffies(timer_slack_val);
		del_timer(&pcpu->cpu_slack_timer);
		pcpu->cpu_slack_timer.expires = expires;
		add_timer_on(&pcpu->cpu_slack_timer, cpu);
	}

	spin_unlock_irqrestore(&pcpu->load_lock, flags);
}

/* The caller shall take enable_sem write semaphore to avoid any timer race.
 * The cpu_timer and cpu_slack_timer must be deactivated when calling this
 * function.
 */
static void cpufreq_interactive_timer_start(int cpu)
{
	struct cpufreq_interactive_cpuinfo *pcpu = &per_cpu(cpuinfo, cpu);
	u64 expires = round_to_nw_start(pcpu->last_evaluated_jiffy);
	unsigned long flags;
	u64 now = ktime_to_us(ktime_get());

	spin_lock_irqsave(&pcpu->load_lock, flags);
	pcpu->cpu_timer.expires = expires;
	add_timer_on(&pcpu->cpu_timer, cpu);
	if (timer_slack_val >= 0 &&
	    (pcpu->target_freq > pcpu->policy->min ||
		(pcpu->target_freq == pcpu->policy->min &&
		  (now < boostpulse_endtime ||
		   now < input_boostpulse_endtime)))) {
		expires += usecs_to_jiffies(timer_slack_val);
		pcpu->cpu_slack_timer.expires = expires;
		add_timer_on(&pcpu->cpu_slack_timer, cpu);
	}

	pcpu->time_in_idle =
		get_cpu_idle_time(cpu, &pcpu->time_in_idle_timestamp,
				  io_is_busy);
	pcpu->cputime_speedadj = 0;
	pcpu->cputime_speedadj_timestamp = pcpu->time_in_idle_timestamp;
	spin_unlock_irqrestore(&pcpu->load_lock, flags);
}

static unsigned int freq_to_above_hispeed_delay(unsigned int freq)
{
	int i;
	unsigned int ret;
	unsigned long flags;

	spin_lock_irqsave(&above_hispeed_delay_lock, flags);

	for (i = 0; i < nabove_hispeed_delay - 1 &&
			freq >= above_hispeed_delay[i+1]; i += 2)
		;

	ret = above_hispeed_delay[i];

	spin_unlock_irqrestore(&above_hispeed_delay_lock, flags);
	return ret;
}

static unsigned int freq_to_targetload(unsigned int freq)
{
	int i;
	unsigned int ret;
	unsigned long flags;

	spin_lock_irqsave(&target_loads_lock, flags);

	for (i = 0; i < ntarget_loads - 1 && freq >= target_loads[i+1]; i += 2)
		;

	ret = target_loads[i];
	spin_unlock_irqrestore(&target_loads_lock, flags);
	return ret;
}

/*
 * If increasing frequencies never map to a lower target load then
 * choose_freq() will find the minimum frequency that does not exceed its
 * target load given the current load.
 */

static unsigned int choose_freq(
	struct cpufreq_interactive_cpuinfo *pcpu, unsigned int loadadjfreq)
{
	unsigned int freq = pcpu->policy->cur;
	unsigned int prevfreq, freqmin, freqmax;
	unsigned int tl;
	int index;

	freqmin = 0;
	freqmax = UINT_MAX;

	do {
		prevfreq = freq;
		tl = freq_to_targetload(freq);

		/*
		 * Find the frequency where the computed load is the closest
		 * to the target load.
		 */

		if (cpufreq_frequency_table_target(
			    pcpu->policy, pcpu->freq_table, loadadjfreq / tl,
			    CPUFREQ_RELATION_C, &index))
			break;
		freq = pcpu->freq_table[index].frequency;

		if (freq > prevfreq) {
			/* The previous frequency is too low. */
			freqmin = prevfreq;

			if (freq >= freqmax) {
				/*
				 * Find the highest frequency that is less
				 * than freqmax.
				 */
				if (cpufreq_frequency_table_target(
					    pcpu->policy, pcpu->freq_table,
					    freqmax - 1, CPUFREQ_RELATION_H,
					    &index))
					break;
				freq = pcpu->freq_table[index].frequency;

				if (freq == freqmin) {
					/*
					 * The first frequency below freqmax
					 * has already been found to be too
					 * low.  freqmax is the lowest speed
					 * we found that is fast enough.
					 */
					freq = freqmax;
					break;
				}
			}
		} else if (freq < prevfreq) {
			/* The previous frequency is high enough. */
			freqmax = prevfreq;

			if (freq <= freqmin) {
				/*
				 * Find the lowest frequency that is higher
				 * than freqmin.
				 */
				if (cpufreq_frequency_table_target(
					    pcpu->policy, pcpu->freq_table,
					    freqmin + 1, CPUFREQ_RELATION_L,
					    &index))
					break;
				freq = pcpu->freq_table[index].frequency;

				/*
				 * If freqmax is the first frequency above
				 * freqmin then we have already found that
				 * this speed is fast enough.
				 */
				if (freq == freqmax)
					break;
			}
		}

		/* If same frequency chosen as previous then done. */
	} while (freq != prevfreq);

	if (go_up_to_ideal_freq &&
	    (freq > pcpu->policy->cur) && (pcpu->policy->cur < ideal_freq)) {
		freq = ideal_freq;
		return freq;
	}

	if (go_down_to_ideal_freq &&
	    (freq < pcpu->policy->cur) && (pcpu->policy->cur > ideal_freq)) {
		freq = ideal_freq;
		return freq;
	}

	if (ramp_up_step && (freq > pcpu->policy->cur)) {
		if (cpufreq_frequency_table_target(
			    pcpu->policy, pcpu->freq_table,
			    pcpu->policy->cur + ramp_up_step,
			    CPUFREQ_RELATION_C, &index))
			return freq;
		else
			freq = pcpu->freq_table[index].frequency;

		if (freq <= pcpu->policy->cur) {
			if (cpufreq_frequency_table_target(
				    pcpu->policy, pcpu->freq_table,
				    pcpu->policy->cur + ramp_up_step,
				    CPUFREQ_RELATION_L, &index))
				return freq;
			else
				freq = pcpu->freq_table[index].frequency;
		}
	} else if (ramp_down_step && (freq < pcpu->policy->cur)) {
		if (cpufreq_frequency_table_target(
			    pcpu->policy, pcpu->freq_table,
			    pcpu->policy->cur - ramp_down_step,
#if CPUFREQ_RELATION_C == CPUFREQ_RELATION_L
			    CPUFREQ_RELATION_H, &index))
#else
			    CPUFREQ_RELATION_C, &index))
#endif
			return freq;
		else
			freq = pcpu->freq_table[index].frequency;

		if (freq >= pcpu->policy->cur) {
			if (cpufreq_frequency_table_target(
				    pcpu->policy, pcpu->freq_table,
				    pcpu->policy->cur - ramp_down_step,
				    CPUFREQ_RELATION_H, &index))
				return freq;
			else
				freq = pcpu->freq_table[index].frequency;
		}
	}

	return freq;
}

static u64 update_load(int cpu)
{
	struct cpufreq_interactive_cpuinfo *pcpu = &per_cpu(cpuinfo, cpu);
	u64 now;
	u64 now_idle;
	unsigned int delta_idle;
	unsigned int delta_time;
	u64 active_time;

	now_idle = get_cpu_idle_time(cpu, &now, io_is_busy);
	delta_idle = (unsigned int)(now_idle - pcpu->time_in_idle);
	delta_time = (unsigned int)(now - pcpu->time_in_idle_timestamp);

	if (delta_time <= delta_idle)
		active_time = 0;
	else
		active_time = delta_time - delta_idle;

	pcpu->cputime_speedadj += active_time * pcpu->policy->cur;

	pcpu->time_in_idle = now_idle;
	pcpu->time_in_idle_timestamp = now;
	return now;
}

static void cpufreq_interactive_timer(unsigned long data)
{
	u64 now;
	unsigned int delta_time;
	u64 cputime_speedadj;
	int cpu_load;
	struct cpufreq_interactive_cpuinfo *pcpu =
		&per_cpu(cpuinfo, data);
	unsigned int new_freq;
	unsigned int loadadjfreq;
	unsigned int index;
	unsigned long flags;
	unsigned int this_hispeed_freq;
	bool boosted;
	bool touched;
	int i, max_load;
	unsigned int max_freq;
	struct cpufreq_interactive_cpuinfo *picpu;
	static unsigned int inter_staycycles;

	if (!down_read_trylock(&pcpu->enable_sem))
		return;
	if (!pcpu->governor_enabled)
		goto exit;

	if (cpu_is_offline(data))
		goto exit;

	spin_lock_irqsave(&pcpu->load_lock, flags);
	now = update_load(data);
	delta_time = (unsigned int)(now - pcpu->cputime_speedadj_timestamp);
	cputime_speedadj = pcpu->cputime_speedadj;
	pcpu->last_evaluated_jiffy = get_jiffies_64();
	spin_unlock_irqrestore(&pcpu->load_lock, flags);

	if (WARN_ON_ONCE(!delta_time))
		goto rearm;

	spin_lock_irqsave(&pcpu->target_freq_lock, flags);
	do_div(cputime_speedadj, delta_time);
	loadadjfreq = (unsigned int)cputime_speedadj * 100;
	cpu_load = loadadjfreq / pcpu->policy->cur;
	pcpu->prev_load = cpu_load;
	touched = now < input_boostpulse_endtime;
	boosted = boost_val || now < boostpulse_endtime || touched;
	this_hispeed_freq = max(hispeed_freq, pcpu->policy->min);

	if (cpu_load >= go_hispeed_load) {
		if (pcpu->policy->cur < this_hispeed_freq) {
			new_freq = this_hispeed_freq;
		} else {
			new_freq = choose_freq(pcpu, loadadjfreq);

			if (plus_ondemand) {
				inter_staycycles++;
				if (inter_staycycles >= plus_ondemand)
					new_freq = pcpu->policy->max;
			}

			if (new_freq < this_hispeed_freq)
				new_freq = this_hispeed_freq;
		}
	} else if (cpu_load <= go_lospeed_load && !boost_val) {
		boosted = false;
		input_boostpulse_endtime = now;
		pcpu->max_freq_hyst_start_time -= max_freq_hysteresis;
		new_freq = pcpu->policy->min;
		pcpu->floor_freq = new_freq;
		pcpu->floor_validate_time = now;
		inter_staycycles = 0;
	} else {
		new_freq = choose_freq(pcpu, loadadjfreq);
		if (new_freq > this_hispeed_freq &&
				pcpu->target_freq < this_hispeed_freq)
			new_freq = this_hispeed_freq;

		if (sync_freq && new_freq < sync_freq) {

			max_load = 0;
			max_freq = 0;

			for_each_online_cpu(i) {
				picpu = &per_cpu(cpuinfo, i);

				if (i == data || picpu->prev_load <
						up_threshold_any_cpu_load)
					continue;

				max_load = max(max_load, picpu->prev_load);
				max_freq = max(max_freq, picpu->target_freq);
			}

			if (max_freq > up_threshold_any_cpu_freq &&
				max_load >= up_threshold_any_cpu_load)
				new_freq = sync_freq;
		}
		inter_staycycles = 0;
	}

	if (boosted) {
		if (touched)
			this_hispeed_freq = max(input_boost_freq, pcpu->policy->min);
		new_freq = max(new_freq, this_hispeed_freq);
	}

	if (pcpu->policy->cur >= this_hispeed_freq &&
	    new_freq > pcpu->policy->cur &&
	    now - pcpu->hispeed_validate_time <
	    freq_to_above_hispeed_delay(pcpu->policy->cur)) {
		spin_unlock_irqrestore(&pcpu->target_freq_lock, flags);
		goto rearm;
	}

	pcpu->local_hvtime = now;

	if (powersave_bias)
		new_freq -= (int)new_freq * powersave_bias / 1000;

	if (cpufreq_frequency_table_target(pcpu->policy, pcpu->freq_table,
					   new_freq, CPUFREQ_RELATION_C,
					   &index)) {
		spin_unlock_irqrestore(&pcpu->target_freq_lock, flags);
		goto rearm;
	}

	new_freq = pcpu->freq_table[index].frequency;

	if (new_freq < pcpu->target_freq &&
	    now - pcpu->max_freq_hyst_start_time < max_freq_hysteresis) {
		spin_unlock_irqrestore(&pcpu->target_freq_lock, flags);
		goto rearm;
	}

	/*
	 * Do not scale below floor_freq unless we have been at or above the
	 * floor frequency for the minimum sample time since last validated.
	 */
	if (new_freq < pcpu->floor_freq) {
		if (now - pcpu->floor_validate_time < min_sample_time) {
			spin_unlock_irqrestore(&pcpu->target_freq_lock, flags);
			goto rearm;
		}
	}

	/*
	 * Update the timestamp for checking whether speed has been held at
	 * or above the selected frequency for a minimum of min_sample_time,
	 * if not boosted to this_hispeed_freq.  If boosted to this_hispeed_freq
	 * then we allow the speed to drop as soon as the boostpulse duration
	 * expires (or the indefinite boost is turned off).
	 */

	if (!boosted || new_freq > this_hispeed_freq) {
		pcpu->floor_freq = new_freq;
		pcpu->floor_validate_time = now;
	}

	if (new_freq == pcpu->policy->max)
		pcpu->max_freq_hyst_start_time = now;

	if (pcpu->target_freq == new_freq &&
			pcpu->target_freq <= pcpu->policy->cur) {
		spin_unlock_irqrestore(&pcpu->target_freq_lock, flags);
		goto rearm;
	}

	pcpu->target_freq = new_freq;
	spin_unlock_irqrestore(&pcpu->target_freq_lock, flags);
	spin_lock_irqsave(&speedchange_cpumask_lock, flags);
	cpumask_set_cpu(data, &speedchange_cpumask);
	spin_unlock_irqrestore(&speedchange_cpumask_lock, flags);
	wake_up_process(speedchange_task);

rearm:
	if (!timer_pending(&pcpu->cpu_timer))
		cpufreq_interactive_timer_resched(data, false);

exit:
	up_read(&pcpu->enable_sem);
	return;
}

static void cpufreq_interactive_idle_end(void)
{
	struct cpufreq_interactive_cpuinfo *pcpu =
		&per_cpu(cpuinfo, smp_processor_id());

	if (!down_read_trylock(&pcpu->enable_sem))
		return;
	if (!pcpu->governor_enabled) {
		up_read(&pcpu->enable_sem);
		return;
	}

	/* Arm the timer for 1-2 ticks later if not already. */
	if (!timer_pending(&pcpu->cpu_timer)) {
		cpufreq_interactive_timer_resched(smp_processor_id(), false);
	} else if (time_after_eq(jiffies, pcpu->cpu_timer.expires)) {
		del_timer(&pcpu->cpu_timer);
		del_timer(&pcpu->cpu_slack_timer);
		cpufreq_interactive_timer(smp_processor_id());
	}

	up_read(&pcpu->enable_sem);
}

static int cpufreq_interactive_speedchange_task(void *data)
{
	unsigned int cpu;
	cpumask_t tmp_mask;
	unsigned long flags;
	struct cpufreq_interactive_cpuinfo *pcpu;

	while (1) {
		set_current_state(TASK_INTERRUPTIBLE);
		spin_lock_irqsave(&speedchange_cpumask_lock, flags);

		if (cpumask_empty(&speedchange_cpumask)) {
			spin_unlock_irqrestore(&speedchange_cpumask_lock,
					       flags);
			schedule();

			if (kthread_should_stop())
				break;

			spin_lock_irqsave(&speedchange_cpumask_lock, flags);
		}

		set_current_state(TASK_RUNNING);
		tmp_mask = speedchange_cpumask;
		cpumask_clear(&speedchange_cpumask);
		spin_unlock_irqrestore(&speedchange_cpumask_lock, flags);

		for_each_cpu(cpu, &tmp_mask) {
			unsigned int j;
			unsigned int max_freq = 0;
			struct cpufreq_interactive_cpuinfo *pjcpu;
			u64 hvt = 0;

			pcpu = &per_cpu(cpuinfo, cpu);
			if (!down_read_trylock(&pcpu->enable_sem))
				continue;
			if (!pcpu->governor_enabled) {
				up_read(&pcpu->enable_sem);
				continue;
			}

			for_each_cpu(j, pcpu->policy->cpus) {
				pjcpu = &per_cpu(cpuinfo, j);

				if (pjcpu->target_freq > max_freq) {
					max_freq = pjcpu->target_freq;
					hvt = pjcpu->local_hvtime;
				} else if (pjcpu->target_freq == max_freq) {
					hvt = min(hvt, pjcpu->local_hvtime);
				}
			}

			if (max_freq != pcpu->policy->cur) {
				__cpufreq_driver_target(pcpu->policy,
							max_freq,
							CPUFREQ_RELATION_H);
				for_each_cpu(j, pcpu->policy->cpus) {
					pjcpu = &per_cpu(cpuinfo, j);
					pjcpu->hispeed_validate_time = hvt;
				}
			}

			up_read(&pcpu->enable_sem);
		}
	}

	return 0;
}

static void cpufreq_interactive_boost(void)
{
	int i;
	int anyboost = 0;
	unsigned long flags[2];
	struct cpufreq_interactive_cpuinfo *pcpu;

	spin_lock_irqsave(&speedchange_cpumask_lock, flags[0]);

	for_each_online_cpu(i) {
		pcpu = &per_cpu(cpuinfo, i);
		spin_lock_irqsave(&pcpu->target_freq_lock, flags[1]);
		if (pcpu->target_freq < hispeed_freq) {
			pcpu->target_freq = hispeed_freq;
			cpumask_set_cpu(i, &speedchange_cpumask);
			pcpu->hispeed_validate_time =
				ktime_to_us(ktime_get());
			anyboost = 1;
		}

		/*
		 * Set floor freq and (re)start timer for when last
		 * validated.
		 */

		pcpu->floor_freq = hispeed_freq;
		pcpu->floor_validate_time = ktime_to_us(ktime_get());
		spin_unlock_irqrestore(&pcpu->target_freq_lock, flags[1]);
	}

	spin_unlock_irqrestore(&speedchange_cpumask_lock, flags[0]);

	if (anyboost)
		wake_up_process(speedchange_task);
}

static int cpufreq_interactive_notifier(
	struct notifier_block *nb, unsigned long val, void *data)
{
	struct cpufreq_freqs *freq = data;
	struct cpufreq_interactive_cpuinfo *pcpu;
	int cpu;
	unsigned long flags;

	if (val == CPUFREQ_PRECHANGE) {
		pcpu = &per_cpu(cpuinfo, freq->cpu);
		if (!down_read_trylock(&pcpu->enable_sem))
			return 0;
		if (!pcpu->governor_enabled) {
			up_read(&pcpu->enable_sem);
			return 0;
		}

		for_each_cpu(cpu, pcpu->policy->cpus) {
			struct cpufreq_interactive_cpuinfo *pjcpu =
				&per_cpu(cpuinfo, cpu);
			if (cpu != freq->cpu) {
				if (!down_read_trylock(&pjcpu->enable_sem))
					continue;
				if (!pjcpu->governor_enabled) {
					up_read(&pjcpu->enable_sem);
					continue;
				}
			}
			spin_lock_irqsave(&pjcpu->load_lock, flags);
			update_load(cpu);
			spin_unlock_irqrestore(&pjcpu->load_lock, flags);
			if (cpu != freq->cpu)
				up_read(&pjcpu->enable_sem);
		}

		up_read(&pcpu->enable_sem);
	}
	return 0;
}

static struct notifier_block cpufreq_notifier_block = {
	.notifier_call = cpufreq_interactive_notifier,
};

static unsigned int *get_tokenized_data(const char *buf, int *num_tokens)
{
	const char *cp;
	int i;
	int ntokens = 1;
	unsigned int *tokenized_data;
	int err = -EINVAL;

	cp = buf;
	while ((cp = strpbrk(cp + 1, " :")))
		ntokens++;

	if (!(ntokens & 0x1))
		goto err;

	tokenized_data = kmalloc(ntokens * sizeof(unsigned int), GFP_KERNEL);
	if (!tokenized_data) {
		err = -ENOMEM;
		goto err;
	}

	cp = buf;
	i = 0;
	while (i < ntokens) {
		if (sscanf(cp, "%u", &tokenized_data[i++]) != 1)
			goto err_kfree;

		cp = strpbrk(cp, " :");
		if (!cp)
			break;
		cp++;
	}

	if (i != ntokens)
		goto err_kfree;

	*num_tokens = ntokens;
	return tokenized_data;

err_kfree:
	kfree(tokenized_data);
err:
	return ERR_PTR(err);
}

static ssize_t show_target_loads(
	struct kobject *kobj, struct attribute *attr, char *buf)
{
	int i;
	ssize_t ret = 0;
	unsigned long flags;

	spin_lock_irqsave(&target_loads_lock, flags);

	for (i = 0; i < ntarget_loads; i++)
		ret += sprintf(buf + ret, "%u%s", target_loads[i],
			       i & 0x1 ? ":" : " ");

	sprintf(buf + ret - 1, "\n");
	spin_unlock_irqrestore(&target_loads_lock, flags);
	return ret;
}

static ssize_t store_target_loads(
	struct kobject *kobj, struct attribute *attr, const char *buf,
	size_t count)
{
	int ntokens;
	unsigned int *new_target_loads = NULL;
	unsigned long flags;

	new_target_loads = get_tokenized_data(buf, &ntokens);
	if (IS_ERR(new_target_loads))
		return PTR_RET(new_target_loads);

	spin_lock_irqsave(&target_loads_lock, flags);
	if (target_loads != default_target_loads)
		kfree(target_loads);
	target_loads = new_target_loads;
	ntarget_loads = ntokens;
	spin_unlock_irqrestore(&target_loads_lock, flags);
	return count;
}

static struct global_attr target_loads_attr =
	__ATTR(target_loads, S_IRUGO | S_IWUSR,
		show_target_loads, store_target_loads);

static ssize_t show_above_hispeed_delay(
	struct kobject *kobj, struct attribute *attr, char *buf)
{
	int i;
	ssize_t ret = 0;
	unsigned long flags;

	spin_lock_irqsave(&above_hispeed_delay_lock, flags);

	for (i = 0; i < nabove_hispeed_delay; i++)
		ret += sprintf(buf + ret, "%u%s", above_hispeed_delay[i],
			       i & 0x1 ? ":" : " ");

	sprintf(buf + ret - 1, "\n");
	spin_unlock_irqrestore(&above_hispeed_delay_lock, flags);
	return ret;
}

static ssize_t store_above_hispeed_delay(
	struct kobject *kobj, struct attribute *attr, const char *buf,
	size_t count)
{
	int ntokens, i;
	unsigned int *new_above_hispeed_delay = NULL;
	unsigned long flags;

	new_above_hispeed_delay = get_tokenized_data(buf, &ntokens);
	if (IS_ERR(new_above_hispeed_delay))
		return PTR_RET(new_above_hispeed_delay);

	/* Make sure frequencies are in ascending order. */
	for (i = 3; i < ntokens; i += 2) {
		if (new_above_hispeed_delay[i] <=
		    new_above_hispeed_delay[i - 2]) {
			kfree(new_above_hispeed_delay);
			return -EINVAL;
		}
	}

	spin_lock_irqsave(&above_hispeed_delay_lock, flags);
	if (above_hispeed_delay != default_above_hispeed_delay)
		kfree(above_hispeed_delay);
	above_hispeed_delay = new_above_hispeed_delay;
	nabove_hispeed_delay = ntokens;
	spin_unlock_irqrestore(&above_hispeed_delay_lock, flags);
	return count;

}

static struct global_attr above_hispeed_delay_attr =
	__ATTR(above_hispeed_delay, S_IRUGO | S_IWUSR,
		show_above_hispeed_delay, store_above_hispeed_delay);

static ssize_t show_hispeed_freq(struct kobject *kobj,
				 struct attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", hispeed_freq);
}

static ssize_t store_hispeed_freq(struct kobject *kobj,
				  struct attribute *attr, const char *buf,
				  size_t count)
{
	int ret;
	long unsigned int val;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	hispeed_freq = val;
	return count;
}

static struct global_attr hispeed_freq_attr = __ATTR(hispeed_freq, 0644,
		show_hispeed_freq, store_hispeed_freq);

static ssize_t show_input_boost_freq(struct kobject *kobj,
				 struct attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", input_boost_freq);
}

static ssize_t store_input_boost_freq(struct kobject *kobj,
				  struct attribute *attr, const char *buf,
				  size_t count)
{
	int ret;
	long unsigned int val;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	input_boost_freq = val;
	return count;
}

static struct global_attr input_boost_freq_attr = __ATTR(input_boost_freq, 0644,
		show_input_boost_freq, store_input_boost_freq);

static ssize_t show_ideal_freq(struct kobject *kobj,
			struct attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", ideal_freq);
}

static ssize_t store_ideal_freq(struct kobject *kobj,
			struct attribute *attr, const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	ideal_freq = val;
	return count;
}

static struct global_attr ideal_freq_attr = __ATTR(ideal_freq, 0644,
		show_ideal_freq, store_ideal_freq);

static ssize_t show_go_up_to_ideal_freq(struct kobject *kobj,
				     struct attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", go_up_to_ideal_freq);
}

static ssize_t store_go_up_to_ideal_freq(struct kobject *kobj,
			struct attribute *attr, const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	go_up_to_ideal_freq = val;
	return count;
}

static struct global_attr go_up_to_ideal_freq_attr = __ATTR(go_up_to_ideal_freq, 0644,
		show_go_up_to_ideal_freq, store_go_up_to_ideal_freq);

static ssize_t show_go_down_to_ideal_freq(struct kobject *kobj,
				     struct attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", go_down_to_ideal_freq);
}

static ssize_t store_go_down_to_ideal_freq(struct kobject *kobj,
			struct attribute *attr, const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	go_down_to_ideal_freq = val;
	return count;
}

static struct global_attr go_down_to_ideal_freq_attr = __ATTR(go_down_to_ideal_freq, 0644,
		show_go_down_to_ideal_freq, store_go_down_to_ideal_freq);

static ssize_t show_max_freq_hysteresis(struct kobject *kobj,
				     struct attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", max_freq_hysteresis);
}

static ssize_t store_max_freq_hysteresis(struct kobject *kobj,
			struct attribute *attr, const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	max_freq_hysteresis = val;
	return count;
}

static struct global_attr max_freq_hysteresis_attr =
	__ATTR(max_freq_hysteresis, 0644, show_max_freq_hysteresis,
		store_max_freq_hysteresis);

static ssize_t show_align_windows(struct kobject *kobj,
				     struct attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", align_windows);
}

static ssize_t store_align_windows(struct kobject *kobj,
			struct attribute *attr, const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	align_windows = val;
	return count;
}

static struct global_attr align_windows_attr = __ATTR(align_windows, 0644,
		show_align_windows, store_align_windows);

static ssize_t show_go_hispeed_load(struct kobject *kobj,
				     struct attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", go_hispeed_load);
}

static ssize_t store_go_hispeed_load(struct kobject *kobj,
			struct attribute *attr, const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	go_hispeed_load = val;
	return count;
}

static struct global_attr go_hispeed_load_attr = __ATTR(go_hispeed_load, 0644,
		show_go_hispeed_load, store_go_hispeed_load);

static ssize_t show_go_lospeed_load(struct kobject *kobj,
				     struct attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", go_lospeed_load);
}

static ssize_t store_go_lospeed_load(struct kobject *kobj,
			struct attribute *attr, const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	go_lospeed_load = val;
	return count;
}

static struct global_attr go_lospeed_load_attr = __ATTR(go_lospeed_load, 0644,
		show_go_lospeed_load, store_go_lospeed_load);

static ssize_t show_min_sample_time(struct kobject *kobj,
				struct attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", min_sample_time);
}

static ssize_t store_min_sample_time(struct kobject *kobj,
			struct attribute *attr, const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	min_sample_time = val;
	return count;
}

static struct global_attr min_sample_time_attr = __ATTR(min_sample_time, 0644,
		show_min_sample_time, store_min_sample_time);

static ssize_t show_timer_rate(struct kobject *kobj,
			struct attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", timer_rate);
}

static ssize_t store_timer_rate(struct kobject *kobj,
			struct attribute *attr, const char *buf, size_t count)
{
	int ret;
	unsigned long val, val_round;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;

	val_round = jiffies_to_usecs(usecs_to_jiffies(val));
	if (val != val_round)
		pr_warn("timer_rate not aligned to jiffy. Rounded up to %lu\n",
				val_round);

	timer_rate = val_round;
	return count;
}

static struct global_attr timer_rate_attr = __ATTR(timer_rate, 0644,
		show_timer_rate, store_timer_rate);

static ssize_t show_timer_slack(
	struct kobject *kobj, struct attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", timer_slack_val);
}

static ssize_t store_timer_slack(
	struct kobject *kobj, struct attribute *attr, const char *buf,
	size_t count)
{
	int ret;
	unsigned long val;

	ret = kstrtol(buf, 10, &val);
	if (ret < 0)
		return ret;

	timer_slack_val = val;
	return count;
}

define_one_global_rw(timer_slack);

static ssize_t show_boost(struct kobject *kobj, struct attribute *attr,
			  char *buf)
{
	return sprintf(buf, "%d\n", boost_val);
}

static ssize_t store_boost(struct kobject *kobj, struct attribute *attr,
			   const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;

	boost_val = val;

	if (boost_val)
		cpufreq_interactive_boost();
	else
		boostpulse_endtime = ktime_to_us(ktime_get());

	return count;
}

define_one_global_rw(boost);

static ssize_t store_boostpulse(struct kobject *kobj, struct attribute *attr,
				const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;

	boostpulse_endtime = ktime_to_us(ktime_get()) + boostpulse_duration_val;
	cpufreq_interactive_boost();
	return count;
}

static struct global_attr boostpulse =
	__ATTR(boostpulse, 0200, NULL, store_boostpulse);

static ssize_t show_boostpulse_duration(
	struct kobject *kobj, struct attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", boostpulse_duration_val);
}

static ssize_t store_boostpulse_duration(
	struct kobject *kobj, struct attribute *attr, const char *buf,
	size_t count)
{
	int ret;
	unsigned long val;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;

	boostpulse_duration_val = val;
	return count;
}

define_one_global_rw(boostpulse_duration);

static ssize_t show_input_boostpulse_duration(
	struct kobject *kobj, struct attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", input_boostpulse_duration_val);
}

static ssize_t store_input_boostpulse_duration(
	struct kobject *kobj, struct attribute *attr, const char *buf,
	size_t count)
{
	int ret;
	unsigned long val;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;

	input_boostpulse_duration_val = val;
	return count;
}

define_one_global_rw(input_boostpulse_duration);

static ssize_t show_io_is_busy(struct kobject *kobj,
			struct attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", io_is_busy);
}

static ssize_t store_io_is_busy(struct kobject *kobj,
			struct attribute *attr, const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	io_is_busy = val;
	return count;
}

static struct global_attr io_is_busy_attr = __ATTR(io_is_busy, 0644,
		show_io_is_busy, store_io_is_busy);

static ssize_t show_powersave_bias(struct kobject *kobj,
				     struct attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", powersave_bias);
}

static ssize_t store_powersave_bias(struct kobject *kobj,
			struct attribute *attr, const char *buf, size_t count)
{
	int ret;
	int val;

	ret = kstrtoint(buf, 0, &val);
	if (ret < 0)
		return ret;

	if (val > POWERSAVE_BIAS_MAXLEVEL)
		val = POWERSAVE_BIAS_MAXLEVEL;
	else if (val < POWERSAVE_BIAS_MINLEVEL)
		val = POWERSAVE_BIAS_MINLEVEL;

	powersave_bias = val;
	return count;
}

static struct global_attr powersave_bias_attr = __ATTR(powersave_bias, 0644,
		show_powersave_bias, store_powersave_bias);

static ssize_t show_sync_freq(struct kobject *kobj,
			struct attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", sync_freq);
}

static ssize_t store_sync_freq(struct kobject *kobj,
			struct attribute *attr, const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	sync_freq = val;
	return count;
}

static struct global_attr sync_freq_attr = __ATTR(sync_freq, 0644,
		show_sync_freq, store_sync_freq);

static ssize_t show_up_threshold_any_cpu_load(struct kobject *kobj,
			struct attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n", up_threshold_any_cpu_load);
}

static ssize_t store_up_threshold_any_cpu_load(struct kobject *kobj,
			struct attribute *attr, const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	up_threshold_any_cpu_load = val;
	return count;
}

static struct global_attr up_threshold_any_cpu_load_attr =
		__ATTR(up_threshold_any_cpu_load, 0644,
		show_up_threshold_any_cpu_load,
				store_up_threshold_any_cpu_load);

static ssize_t show_up_threshold_any_cpu_freq(struct kobject *kobj,
			struct attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n", up_threshold_any_cpu_freq);
}

static ssize_t store_up_threshold_any_cpu_freq(struct kobject *kobj,
			struct attribute *attr, const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	up_threshold_any_cpu_freq = val;
	return count;
}

static struct global_attr up_threshold_any_cpu_freq_attr =
		__ATTR(up_threshold_any_cpu_freq, 0644,
		show_up_threshold_any_cpu_freq,
				store_up_threshold_any_cpu_freq);

static ssize_t show_plus_ondemand(struct kobject *kobj,
			struct attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", plus_ondemand);
}

static ssize_t store_plus_ondemand(struct kobject *kobj,
			struct attribute *attr, const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	plus_ondemand = val;
	return count;
}

static struct global_attr plus_ondemand_attr = __ATTR(plus_ondemand, 0644,
		show_plus_ondemand, store_plus_ondemand);

static ssize_t show_ramp_up_step(struct kobject *kobj,
			struct attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", ramp_up_step);
}

static ssize_t store_ramp_up_step(struct kobject *kobj,
			struct attribute *attr, const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	ramp_up_step = val;
	return count;
}

static struct global_attr ramp_up_step_attr = __ATTR(ramp_up_step, 0644,
		show_ramp_up_step, store_ramp_up_step);

static ssize_t show_ramp_down_step(struct kobject *kobj,
			struct attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", ramp_down_step);
}

static ssize_t store_ramp_down_step(struct kobject *kobj,
			struct attribute *attr, const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	ramp_down_step = val;
	return count;
}

static struct global_attr ramp_down_step_attr = __ATTR(ramp_down_step, 0644,
		show_ramp_down_step, store_ramp_down_step);

static struct attribute *interactive_attributes[] = {
	&target_loads_attr.attr,
	&above_hispeed_delay_attr.attr,
	&hispeed_freq_attr.attr,
	&input_boost_freq_attr.attr,
	&ideal_freq_attr.attr,
	&go_up_to_ideal_freq_attr.attr,
	&go_down_to_ideal_freq_attr.attr,
	&go_hispeed_load_attr.attr,
	&go_lospeed_load_attr.attr,
	&min_sample_time_attr.attr,
	&timer_rate_attr.attr,
	&timer_slack.attr,
	&boost.attr,
	&boostpulse.attr,
	&boostpulse_duration.attr,
	&input_boostpulse_duration.attr,
	&io_is_busy_attr.attr,
	&max_freq_hysteresis_attr.attr,
	&align_windows_attr.attr,
	&powersave_bias_attr.attr,
	&sync_freq_attr.attr,
	&up_threshold_any_cpu_load_attr.attr,
	&up_threshold_any_cpu_freq_attr.attr,
	&plus_ondemand_attr.attr,
	&ramp_up_step_attr.attr,
	&ramp_down_step_attr.attr,
	NULL,
};

static void interactive_input_event(struct input_handle *handle,
		unsigned int type,
		unsigned int code, int value)
{
	if (input_boostpulse_duration_val && type == EV_SYN && code == SYN_REPORT)
		input_boostpulse_endtime = ktime_to_us(ktime_get()) +
			input_boostpulse_duration_val;
}

static int interactive_input_connect(struct input_handler *handler,
		struct input_dev *dev, const struct input_device_id *id)
{
	struct input_handle *handle;
	int error;

	handle = kzalloc(sizeof(struct input_handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "cpufreq";

	error = input_register_handle(handle);
	if (error)
		goto err2;

	error = input_open_device(handle);
	if (error)
		goto err1;

	return 0;
err1:
	input_unregister_handle(handle);
err2:
	kfree(handle);
	return error;
}

static void interactive_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id interactive_ids[] = {
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			 INPUT_DEVICE_ID_MATCH_ABSBIT,
		.evbit = { BIT_MASK(EV_ABS) },
		.absbit = { [BIT_WORD(ABS_MT_POSITION_X)] =
			    BIT_MASK(ABS_MT_POSITION_X) |
			    BIT_MASK(ABS_MT_POSITION_Y) |
			    BIT_MASK(ABS_MT_TRACKING_ID) },
	}, /* multi-touch touchscreen */
	{
		.flags = INPUT_DEVICE_ID_MATCH_KEYBIT |
			 INPUT_DEVICE_ID_MATCH_ABSBIT,
		.keybit = { [BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH) },
		.absbit = { [BIT_WORD(ABS_X)] =
			    BIT_MASK(ABS_X) | BIT_MASK(ABS_Y) },
	}, /* touchpad */
	{ },
};

static struct input_handler interactive_input_handler = {
	.event		= interactive_input_event,
	.connect	= interactive_input_connect,
	.disconnect	= interactive_input_disconnect,
	.name		= "interest",
	.id_table	= interactive_ids,
};

static struct attribute_group interactive_attr_group = {
	.attrs = interactive_attributes,
	.name = "interest",
};

static int cpufreq_interactive_idle_notifier(struct notifier_block *nb,
					     unsigned long val,
					     void *data)
{
	if (val == IDLE_END)
		cpufreq_interactive_idle_end();

	return 0;
}

static struct notifier_block cpufreq_interactive_idle_nb = {
	.notifier_call = cpufreq_interactive_idle_notifier,
};

static int cpufreq_governor_interest(struct cpufreq_policy *policy,
		unsigned int event)
{
	int rc;
	unsigned int j;
	struct cpufreq_interactive_cpuinfo *pcpu;
	struct cpufreq_frequency_table *freq_table;
	unsigned long flags;
	unsigned int anyboost;

	switch (event) {
	case CPUFREQ_GOV_START:
		if (!cpu_online(policy->cpu))
			return -EINVAL;

		mutex_lock(&gov_lock);

		freq_table =
			cpufreq_frequency_get_table(policy->cpu);
		if (!hispeed_freq)
			hispeed_freq = policy->max;

		for_each_cpu(j, policy->cpus) {
			pcpu = &per_cpu(cpuinfo, j);
			pcpu->policy = policy;
			pcpu->target_freq = policy->cur;
			pcpu->freq_table = freq_table;
			pcpu->floor_freq = pcpu->target_freq;
			pcpu->floor_validate_time =
				ktime_to_us(ktime_get());
			pcpu->hispeed_validate_time =
				pcpu->floor_validate_time;
			pcpu->local_hvtime = pcpu->floor_validate_time;
			pcpu->min_freq = policy->min;
			down_write(&pcpu->enable_sem);
			del_timer_sync(&pcpu->cpu_timer);
			del_timer_sync(&pcpu->cpu_slack_timer);
			pcpu->last_evaluated_jiffy = get_jiffies_64();
			if (cpu_online(j))
				cpufreq_interactive_timer_start(j);
			pcpu->governor_enabled = 1;
			up_write(&pcpu->enable_sem);
		}

		/*
		 * Do not register the idle hook and create sysfs
		 * entries if we have already done so.
		 */
		if (++active_count > 1) {
			mutex_unlock(&gov_lock);
			return 0;
		}

		rc = sysfs_create_group(cpufreq_global_kobject,
				&interactive_attr_group);
		if (rc) {
			mutex_unlock(&gov_lock);
			return rc;
		}

		idle_notifier_register(&cpufreq_interactive_idle_nb);
		cpufreq_register_notifier(
			&cpufreq_notifier_block, CPUFREQ_TRANSITION_NOTIFIER);
		mutex_unlock(&gov_lock);
		break;

	case CPUFREQ_GOV_STOP:
		mutex_lock(&gov_lock);
		for_each_cpu(j, policy->cpus) {
			pcpu = &per_cpu(cpuinfo, j);
			down_write(&pcpu->enable_sem);
			pcpu->governor_enabled = 0;
			pcpu->target_freq = 0;
			del_timer_sync(&pcpu->cpu_timer);
			del_timer_sync(&pcpu->cpu_slack_timer);
			up_write(&pcpu->enable_sem);
		}

		if (--active_count > 0) {
			mutex_unlock(&gov_lock);
			return 0;
		}

		cpufreq_unregister_notifier(
			&cpufreq_notifier_block, CPUFREQ_TRANSITION_NOTIFIER);
		idle_notifier_unregister(&cpufreq_interactive_idle_nb);
		sysfs_remove_group(cpufreq_global_kobject,
				&interactive_attr_group);
		mutex_unlock(&gov_lock);

		break;

	case CPUFREQ_GOV_LIMITS:
		__cpufreq_driver_target(policy,
				policy->cur, CPUFREQ_RELATION_L);
		for_each_cpu(j, policy->cpus) {
			pcpu = &per_cpu(cpuinfo, j);

			down_read(&pcpu->enable_sem);
			if (pcpu->governor_enabled == 0) {
				up_read(&pcpu->enable_sem);
				continue;
			}

			spin_lock_irqsave(&pcpu->target_freq_lock, flags);
			if (policy->max < pcpu->target_freq) {
				pcpu->target_freq = policy->max;
			} else if (policy->min >= pcpu->target_freq) {
				pcpu->target_freq = policy->min;
				anyboost = 1;
			}

			spin_unlock_irqrestore(&pcpu->target_freq_lock, flags);

			if (policy->min < pcpu->min_freq)
				cpufreq_interactive_timer_resched(j, true);
			pcpu->min_freq = policy->min;

			up_read(&pcpu->enable_sem);

			if (anyboost) {
				u64 now = ktime_to_us(ktime_get());

				cpumask_set_cpu(j, &speedchange_cpumask);
				pcpu->hispeed_validate_time = now;
				pcpu->floor_freq = policy->min;
				pcpu->floor_validate_time = now;
			}
		}
		if (anyboost)
			wake_up_process(speedchange_task);

		break;
	}
	return 0;
}

#ifndef CONFIG_CPU_FREQ_DEFAULT_GOV_INTEREST
static
#endif
struct cpufreq_governor cpufreq_gov_interest = {
	.name = "interest",
	.governor = cpufreq_governor_interest,
	.max_transition_latency = 10000000,
	.owner = THIS_MODULE,
};

static void cpufreq_interactive_nop_timer(unsigned long data)
{
}

static int __init cpufreq_interest_init(void)
{
	unsigned int i, rc;
	struct cpufreq_interactive_cpuinfo *pcpu;
	struct sched_param param = { .sched_priority = MAX_RT_PRIO-1 };

	/* Initalize per-cpu timers */
	for_each_possible_cpu(i) {
		pcpu = &per_cpu(cpuinfo, i);
		init_timer_deferrable(&pcpu->cpu_timer);
		pcpu->cpu_timer.function = cpufreq_interactive_timer;
		pcpu->cpu_timer.data = i;
		init_timer(&pcpu->cpu_slack_timer);
		pcpu->cpu_slack_timer.function = cpufreq_interactive_nop_timer;
		spin_lock_init(&pcpu->load_lock);
		spin_lock_init(&pcpu->target_freq_lock);
		init_rwsem(&pcpu->enable_sem);
		if (!i)
			rc = input_register_handler(&interactive_input_handler);
	}

	spin_lock_init(&target_loads_lock);
	spin_lock_init(&speedchange_cpumask_lock);
	spin_lock_init(&above_hispeed_delay_lock);
	mutex_init(&gov_lock);
	speedchange_task =
		kthread_create(cpufreq_interactive_speedchange_task, NULL,
			       "cfinterest");
	if (IS_ERR(speedchange_task))
		return PTR_ERR(speedchange_task);

	sched_setscheduler_nocheck(speedchange_task, SCHED_FIFO, &param);
	get_task_struct(speedchange_task);

	/* NB: wake up so the thread does not look hung to the freezer */
	wake_up_process(speedchange_task);

	return cpufreq_register_governor(&cpufreq_gov_interest);
}

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_INTEREST
fs_initcall(cpufreq_interest_init);
#else
module_init(cpufreq_interest_init);
#endif

static void __exit cpufreq_interactive_exit(void)
{
	unsigned int cpu;

	cpufreq_unregister_governor(&cpufreq_gov_interest);
	for_each_possible_cpu(cpu) {
		if(!cpu)
			input_unregister_handler(&interactive_input_handler);
	}
	kthread_stop(speedchange_task);
	put_task_struct(speedchange_task);
	if (above_hispeed_delay != default_above_hispeed_delay)
		kfree(above_hispeed_delay);
}

module_exit(cpufreq_interactive_exit);

MODULE_AUTHOR("Mike Chan <mike@android.com>");
MODULE_AUTHOR("Yusuke Fukutsuka <donsuke.f@gmail.com>");
MODULE_DESCRIPTION("'cpufreq_interest' - A cpufreq governor for "
	"Latency sensitive workloads");
MODULE_LICENSE("GPL");
