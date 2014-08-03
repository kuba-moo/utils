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

static struct distribution *calc_distr_(const u32 *samples, const int n,
					const u32 min, const u32 max)
{
	int i;
	u32 *table;
	u32 table_size = max - min + 1;
	u32 n_distinct = 0;
	struct distribution *distr;

	table = calloc(table_size, sizeof(*table));
	for (i = 0; i < n; i++)
		if (!table[samples[i] - min]++)
			n_distinct++;

	distr = tal_arr(NULL, struct distribution, n_distinct);
	for (i = table_size - 1; i >= 0; i--)
		if (table[i]) {
			n_distinct--;
			distr[n_distinct].val = i + min;
			distr[n_distinct].cnt = table[i];
		}

	assert(!n_distinct);

	free(table);

	return distr;
}

void calc_distr(struct trace *t)
{
	struct distribution *distr =
		calc_distr_(t->samples, t->d->n_samples, t->min, t->max);

	tal_steal(t->d, distr);
	t->distr = distr;
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

void file_dump(struct distribution *distr, u32 n)
{
	u32 i;
	FILE *f;
	static char name[] = "tr0";

	f = fopen(name, "w");
	for (i = 0; i < n; i++)
		fprintf(f, "%u %u\n", distr[i].val, distr[i].cnt);
	fclose(f);

	name[2]++;
}

static inline double f(double x, double a, double s, double m)
{
	const double scaled = (x - m) / s;
	const double powed = pow(scaled, -a);
	double ret = a/s * powed/scaled * exp(-powed);

	return ret;
}

static double frechet(struct distribution *distr, u32 n,
		      double a, double s, double m)
{
	u32 i;
	double sum = 0;

	for (i = n; i > 0; i--)
		sum += distr[i-1].cnt * log(f(distr[i-1].val, a, s, m));

	return sum;
}

static void shake_m(struct trace *t, struct distribution *distr, u32 n,
		    double a, double s, double *m_, const u32 max_retry)
{
	double m = *m_;
	u32 retry = 0;
	double delta = 4;
	double old = frechet(distr, n, a, s, m);
	double less, more;

	less = frechet(distr, n, a, s, m - delta);
	more = frechet(distr, n, a, s, m + delta);

	while (retry < max_retry) {
		if (old < less) {
			dbg("r%02d m=%lf \t%le %+le %+le\n", retry, m, old, less - old, more - old);
			m -= delta;
			more = old;
			old = less;
			less = frechet(distr, n, a, s, m - delta);
		} else if (old < more) {
			dbg("r%02d m=%lf \t%le %+le %+le\n", retry, m, old, less - old, more - old);
			m += delta;
			less = old;
			old = more;
			more = frechet(distr, n, a, s, m + delta);
		} else {
			retry++;
			delta /= 2;

			less = frechet(distr, n, a, s, m - delta);
			more = frechet(distr, n, a, s, m + delta);
		}
	}

	*m_ = m;
}

static bool shake_a(struct distribution *distr, u32 n,
		    double *a_, double s, double m, const u32 max_retry)
{
	double a = *a_;
	bool ret;
	u32 retry = 0;
	double delta = 1;
	double old = frechet(distr, n, a, s, m);
	double less, more;

	less = frechet(distr, n, a - delta, s, m);
	more = frechet(distr, n, a + delta, s, m);

	while (retry < max_retry) {
		if (old < less && a - delta > 0.0000001) {
			dbg("r%02d a=%lf \t%le %+le %+le\n", retry, a, old, less - old, more - old);
			a -= delta;
			more = old;
			old = less;
			less = frechet(distr, n, a - delta, s, m);
		} else if (old < more) {
			dbg("r%02d a=%lf \t%le %+le %+le\n", retry, a, old, less - old, more - old);
			a += delta;
			less = old;
			old = more;
			more = frechet(distr, n, a + delta, s, m);
		} else {
			retry++;
			delta /= 2;

			less = frechet(distr, n, a - delta, s, m);
			more = frechet(distr, n, a + delta, s, m);
		}
	}

	ret = a != *a_;
	*a_ = a;

	return ret;
}

static bool shake_s(struct distribution *distr, u32 n,
		    double a, double *s_, double m, const u32 max_retry)
{
	double s = *s_;
	bool ret;
	u32 retry = 0;
	double delta = 1;
	double old = frechet(distr, n, a, s, m);
	double less, more;

	less = frechet(distr, n, a, s - delta, m);
	more = frechet(distr, n, a, s + delta, m);

	while (retry < max_retry) {
		if (old < less && s - delta > 0.0000001) {
			dbg("r%02d s=%lf \t%le %+le %+le\n", retry, s, old, less - old, more - old);
			s -= delta;
			more = old;
			old = less;
			less = frechet(distr, n, a, s - delta, m);
		} else if (old < more) {
			dbg("r%02d s=%lf \t%le %+le %+le\n", retry, s, old, less - old, more - old);
			s += delta;
			less = old;
			old = more;
			more = frechet(distr, n, a, s + delta, m);
		} else {
			retry++;
			delta /= 2;

			less = frechet(distr, n, a, s - delta, m);
			more = frechet(distr, n, a, s + delta, m);
		}
	}

	ret = s != *s_;
	*s_ = s;
	return ret;
}

static void fit_frechet(struct trace *t, struct distribution *distr, u32 n)
{
	double a = 3, s = t->max - t->min, m = t->min;
	u32 retry = 2;

	while (true) {
		shake_m(t, distr, n, a, s, &m, retry);
		if (shake_a(distr, n, &a, s, m, retry))
			continue;
		if (shake_s(distr, n, a, &s, m, retry))
			continue;

		if (++retry > 5)
			break;
	}

	t->distr_m = m;
	t->distr_s = s;
	t->distr_a = a;
}

#include "chi_2.h"

bool chi_2_test(struct trace *t, u32 n_maxes,
		struct distribution *distr, u32 n)
{
	const u32 b_cnt = n_maxes/30;
	const u32 b_width = (distr[n - 1]->val - distr[0]->val)/b_cnt;
	u32 bucks[b_cnt];
	u32 b, i, b_upper = distr[0]->val + b_width;

	memset(bucks, 0, sizeof(bucks));

	b = i = 0;
	while (true) {
		while (
		bucks[b] += distr[i]->val;
	}

	return true;
}

#define FIT_FRAC 1/8

void calc_gumbel(struct trace *t, u32 n_samples)
{
	u32 i;
	u32 b_s = 6;
	u32 *marr;
	u32 arr_len = n_samples * FIT_FRAC >> b_s;
	size_t marr_size = arr_len * sizeof(*marr);
	u32 n_distinct = t->max - t->min + 1;
	struct distribution *distr;

	marr = memalign(VEC_SZ, marr_size);
	distr = malloc(n_distinct * sizeof(*distr));

	while (true) {
		if (n_samples >> b_s < 32) {
			err("Failed to fit distribution\n");
			break;
		}

		arr_len = n_samples * FIT_FRAC >> b_s;
		marr_size = arr_len * sizeof(*marr);
		memset(marr, 0, marr_size);

		for (i = 0; i < arr_len << b_s; i++)
			if (t->samples[i] > marr[i >> b_s])
				marr[i >> b_s] = t->samples[i];

		qsort(marr, arr_len, sizeof(*marr), cmp_u32);

		distr[0].val = marr[0];
		distr[0].cnt = 1;
		n_distinct = 0;
		for (i = 1; i < arr_len; i++) {
			if (distr[n_distinct].val == marr[i]) {
				distr[n_distinct].cnt++;
			} else {
				n_distinct++;
				distr[n_distinct].cnt = 1;
				distr[n_distinct].val = marr[i];
			}
		}
		n_distinct++;

		fit_frechet(t, distr, n_distinct);
		msg("Frechet (%lg): m=%lf; s=%lf; a=%lf\n",
		    frechet(distr, n_distinct,
			    t->distr_a, t->distr_s, t->distr_m),
		    t->distr_m, t->distr_s, t->distr_a);

		file_dump(distr, n_distinct);

		if (chi_2_test(t, arr_len, distr, n_distinct))
			break;

		b_s <<= 1;
	}

	free(marr);
	free(distr);
}
