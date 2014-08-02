/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 *
 * Copyright (C) 2014 Jakub Kicinski <kubakici@wp.pl>
 */
#ifndef MGR_INTERP_H
#define MGR_INTERP_H 1

#define _GNU_SOURCE 1

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <inttypes.h>

#include <ccan/short_types/short_types.h>

#define FBOLD "\e[1m"
#define FNORM "\e[0m"
#define FGRN  "\e[32m"
#define FYLW  "\e[33m"

#define dbg(fmt...)  if (0) printf(fmt)
#define msg(fmt...)  ({ if (!args.quiet) printf(fmt); })
#define err(fmt...)  fprintf(stderr, fmt)
#define err_ret(fmt...) ({ err(fmt); 1; })
#define err_nret(fmt...) ({ err(fmt); NULL; })
#define perr_ret(msg) ({ perror(msg); 1; })
#define perr_nret(msg) ({ perror(msg); NULL; })

#define PCAP_CNT_INF		-1
#define PCAP_SNAPLEN_ALL	2048

#define us_to_clk(x) ((x)*1000/8)
#define clk_to_us(x) ((x)*8/1000)

struct cmdline_args {
	bool quiet;

	int ifg;
	char *res_pfx;
	char *res_dir;

	char *distr;
	char *hm;
	char *stats;
	int aggr;
};

extern struct cmdline_args args;

struct delay {
	u32 n_samples;
	u32 n_notifs;

	u32 min_sample; /* min over all traces */
	u32 max_sample; /* max over all traces */

	double corr;

	char *fname;

	u32 trace_size_;
	struct trace {
		u64 sum;
		double mean;
		double stdev_sum;
		double stdev;

		u32 *samples;
	} t[3];
};

#define for_each_trace(_delay_, _trace_)			\
	for (u32 macro_t_ = 0;					\
	     _trace_ = &_delay_->t[macro_t_], macro_t_ < 3;	\
	     macro_t_++)					\


struct delay_bank {
	int n; /* count(bank) */

	u32 min_samples; /* min(d->n_samples) */

	struct delay **bank;
};

struct delay *read_delay(const char *fname);

void calc_mean(struct trace *t, u32 n_samples);
void calc_stdev(struct trace *t, u32 n_samples);
void calc_corr(struct delay *d);

#endif
