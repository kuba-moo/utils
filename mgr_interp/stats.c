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

#include "mgr_interp.h"

#include <assert.h>
#include <malloc.h>
#include <math.h>
#include <stdlib.h>

#include <ccan/tal/tal.h>

#define VEC_SZ (1 << 5)
#define VEC_SZ_MASK (VEC_SZ - 1)

static double table_sum(double *darr, u32 n)
{
	u32 i;

	for (; n != 1; n /= 2) {
		for (i = 0; i < n/2; i++)
			darr[i] += darr[n/2 + i];

		/* Don't always add extra to the same element. */
		if (n & 1)
			darr[n/4] += darr[n/2 + 1];
	}

	return darr[0];
}

void calc_mean(struct trace *t, u32 n_samples)
{
	u32 i;

	for (i = 0; i < n_samples; i++)
		t->sum += t->samples[i];

	t->mean = (double)t->sum / n_samples;
}

#define VEC_PREACC 8

static double preacc[VEC_PREACC] __attribute__ ((aligned (VEC_SZ)));

void calc_stdev(struct trace *t, u32 n_samples)
{
	u32 i, j;
	double *darr;

	darr = memalign(VEC_SZ, n_samples/VEC_PREACC * sizeof(double));

	for (i = 0; i < n_samples; i++) {
		preacc[i % VEC_PREACC] = t->samples[i] - t->mean;

		if (i % VEC_PREACC == VEC_PREACC - 1) {
			darr[i / VEC_PREACC] = 0;
			for (j = 0; j < VEC_PREACC; j++)
				darr[i/VEC_PREACC] += preacc[j] * preacc[j];
		}
	}
	for (j = 0; j < i % VEC_PREACC; j++)
		darr[0] += preacc[j] * preacc[j];

	t->stdev_sum = table_sum(darr, n_samples / VEC_PREACC);
	t->stdev = sqrt((double)t->stdev_sum / (n_samples - 1));

	free(darr);
}

void calc_corr(struct delay *d)
{
	u32 i;
	double corr_sum;
	double *darr;
	const u32 darr_len = d->n_samples / VEC_PREACC +
		!!(d->n_samples % VEC_PREACC);
	const size_t darr_size = darr_len * sizeof(double);

	darr = memalign(VEC_SZ, darr_size);
	memset(darr, 0, darr_size);

#define corr_(_tr_, _i_) ((_tr_).samples[_i_] - (_tr_).mean)

	for (i = 0; i < d->n_samples; i++)
		darr[i / VEC_PREACC] += corr_(d->t[0], i) * corr_(d->t[1], i);

	corr_sum = table_sum(darr, darr_len);
	d->corr = corr_sum / (sqrt(d->t[0].stdev_sum) *
			      sqrt(d->t[1].stdev_sum));

	free(darr);
}

static int cmp_u32(const void *a1, const void *a2)
{
	const u32 *u1 = a1, *u2 = a2;
	return *u1 - *u2;
}

void calc_gumbel(struct trace *t, u32 n_samples)
{
	u32 i;
	u32 b, b_s = 6;
	double *darr;
	u32 *marr;
	u32 arr_len = n_samples >> b_s;
	size_t marr_size = arr_len * sizeof(*marr);
	const size_t darr_size = arr_len * sizeof(*darr);

	marr = memalign(VEC_SZ, marr_size);
	darr = memalign(VEC_SZ, darr_size);

	while (true) {
		b = 1 << b_s;

		if (n_samples >> b_s < 32)
			break;

		arr_len = n_samples >> b_s;
		marr_size = arr_len * sizeof(*marr);
		memset(marr, 0, marr_size);

		for (i = 0; i < arr_len << b_s; i++)
			if (t->samples[i] > marr[i >> b_s])
				marr[i >> b_s] = t->samples[i];

		/* Do Q-Q */
		qsort(marr, arr_len, sizeof(*marr), cmp_u32);

		for (i = 0; i < arr_len; i++)
			darr[i] = -log(-log(1 - (double)i / (n_samples / (b + 1))));


		break;

		/* If Chi^2 < 0.05 {
		           save params;
		           break;
		   }
		*/

		b_s <<= 1;
	}

	free(marr);
	free(darr);
}
