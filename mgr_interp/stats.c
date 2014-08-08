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

#define VEC_PREACC 8

#define FIT_FRAC 1

#define CHI_MIN_BUCKETS 6
#define CHI_MIN_IN_BUCKET 5

static inline void file_dump(struct distribution *distr, u32 n)
{
	u32 i;
	FILE *f;
	static char name[] = "tr00";

	f = fopen(name, "w");
	for (i = 0; i < n; i++)
		fprintf(f, "%u %u\n", distr[i].val, distr[i].cnt);
	fclose(f);

	if (name[3] != '9') {
		name[3]++;
	} else {
		name[2]++;
		name[3] = '0';
	}
}


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

void balance_means(struct delay *d)
{
	const int offset = d->t[1].mean - d->t[0].mean;
	u32 i;

	for (i = 0; i < d->n_samples; i++) {
		if (d->t[1].samples[i] - offset < d->t[2].samples[i])
			d->t[2].samples[i] = d->t[1].samples[i] - offset;

		if (d->t[2].samples[i] < d->t[2].min)
			d->t[2].min = d->t[2].samples[i];
	}
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

#define FIT_GUMBEL

#if defined(FIT_FRECHET)
static inline double f(double x, double a, double s, double m)
{
	const double scaled = (x - m) / s;
	const double powed = pow(scaled, -a);

	return a/s * powed/scaled * exp(-powed);
}

static inline double cdf(const struct trace *t, double x)
{
	return exp(-pow((x-t->ed.m)/t->ed.s, -t->ed.a));
}
#elif defined(FIT_GUMBEL)
static inline double f(double x, double a, double s, double m)
{
	const double scaled = (x - m) / s;

	return 1/s * exp(-(scaled + exp(-scaled)));
}

static inline double cdf(const struct trace *t, double x)
{
	return exp(-exp(-(x - t->ed.m)/t->ed.s));
}

static inline double xceed(const struct trace *t, double p)
{
	return t->ed.m - t->ed.s * log(-log(pow(1 - p, t->ed.block_size)));
}
#else
#error Please choose which distribution to fit. Define FIT_GUMBEL or FIT_FRECHET.
#endif

static double fit_quality(struct distribution *distr, u32 n,
			  double a, double s, double m)
{
	u32 i;
	double sum = 0;
	double res;

	for (i = n; i > 0; i--) {
		res = log(f(distr[i-1].val, a, s, m));
		sum += distr[i-1].cnt * res;
	}

	return sum;
}

/* change this define to msg to get fitting steps debug */
#define shg dbg

static void shake_m(struct trace *t, struct distribution *distr, u32 n,
		    double a, double s, double *m_, const u32 max_retry)
{
	double m = *m_;
	u32 retry = 0;
	double delta = 4;
	double old = fit_quality(distr, n, a, s, m);
	double less, more;

	less = fit_quality(distr, n, a, s, m - delta);
	more = fit_quality(distr, n, a, s, m + delta);

	while (retry < max_retry) {
		if (old < less) {
			shg("r%02d m=%lf \t%le %+le %+le\n", retry, m, old, less - old, more - old);
			m -= delta;
			more = old;
			old = less;
			less = fit_quality(distr, n, a, s, m - delta);
		} else if (old < more) {
			shg("r%02d m=%lf \t%le %+le %+le\n", retry, m, old, less - old, more - old);
			m += delta;
			less = old;
			old = more;
			more = fit_quality(distr, n, a, s, m + delta);
		} else {
			retry++;
			delta /= 2;

			less = fit_quality(distr, n, a, s, m - delta);
			more = fit_quality(distr, n, a, s, m + delta);
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
	double old = fit_quality(distr, n, a, s, m);
	double less, more;

	less = fit_quality(distr, n, a - delta, s, m);
	more = fit_quality(distr, n, a + delta, s, m);

	while (retry < max_retry) {
		if (old < less && a - delta > 0.0000001) {
			shg("r%02d a=%lf \t%le %+le %+le\n", retry, a, old, less - old, more - old);
			a -= delta;
			more = old;
			old = less;
			less = fit_quality(distr, n, a - delta, s, m);
		} else if (old < more) {
			shg("r%02d a=%lf \t%le %+le %+le\n", retry, a, old, less - old, more - old);
			a += delta;
			less = old;
			old = more;
			more = fit_quality(distr, n, a + delta, s, m);
		} else {
			retry++;
			delta /= 2;

			less = fit_quality(distr, n, a - delta, s, m);
			more = fit_quality(distr, n, a + delta, s, m);
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
	double old = fit_quality(distr, n, a, s, m);
	double less, more;

	less = fit_quality(distr, n, a, s - delta, m);
	more = fit_quality(distr, n, a, s + delta, m);

	while (retry < max_retry) {
		if (old < less && s - delta > 0.0000001) {
			shg("r%02d s=%lf \t%le %+le %+le\n", retry, s, old, less - old, more - old);
			s -= delta;
			more = old;
			old = less;
			less = fit_quality(distr, n, a, s - delta, m);
		} else if (old < more) {
			shg("r%02d s=%lf \t%le %+le %+le\n", retry, s, old, less - old, more - old);
			s += delta;
			less = old;
			old = more;
			more = fit_quality(distr, n, a, s + delta, m);
		} else {
			retry++;
			delta /= 2;

			less = fit_quality(distr, n, a, s - delta, m);
			more = fit_quality(distr, n, a, s + delta, m);
		}
	}

	ret = s != *s_;
	*s_ = s;
	return ret;
}

static void fit_frechet(struct trace *t, struct distribution *distr, u32 n)
{
	double a = t->ed.a, s = t->ed.s, m = t->ed.m;
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

	t->ed.m = m;
	t->ed.s = s;
	t->ed.a = a;
}

static int chi_2_test(struct trace *t, u32 n_maxes,
		      const struct distribution *distr, u32 n)
{
	u32 b_cnt = n_maxes/30;
	float b_width = (distr[n - 1].val - distr[0].val)/(float)b_cnt;
	u32 bucks[b_cnt];
	u32 b, i, b_upper, b_sum, b_real;
	double chi = 0, Ei, left_p = 0, right_p;

	while (b_width < 0.5) {
		--b_cnt;
		assert(b_cnt);
		b_width = (distr[n - 1].val - distr[0].val)/(float)b_cnt;
	}

	if (b_cnt < CHI_MIN_BUCKETS)
		return 1;

	memset(bucks, 0, sizeof(bucks));

	for (b = i = 0; b < b_cnt; b++) {
		b_upper = distr[0].val + (b + 1) * b_width;

		while (distr[i].val < b_upper)
			bucks[b] += distr[i++].cnt;
	}
	for (; i < n; i++)
		bucks[b_cnt - 1] += distr[i].cnt;

#ifdef DEBUG
	{
		u32 d_sum = 0;

		b_sum = 0;
		for (i = 0; i < n; i++)
			d_sum += distr[i].cnt;

		for (b = 0; b < b_cnt; b++)
			b_sum += bucks[b];

		assert(b_sum == d_sum);
	}
#endif

	b = b_real = 0;
	while (b < b_cnt) {
		b_real++;

		for (b_sum = 0; b_sum < CHI_MIN_IN_BUCKET && b < b_cnt; b++)
			b_sum += bucks[b];

		if (b < b_cnt - 1)
			right_p = cdf(t, distr[0].val + b * b_width);
		else
			right_p = 1;
		Ei = n_maxes * (right_p - left_p);

		chi += (b_sum - Ei) * (b_sum - Ei) / Ei;

		dbg("chi %.2lf b:%u l:%.5lf r:%.5lf Ei:%.2lf Oi:%u\tp:%.5lf\n",
		    chi, b, left_p, right_p, Ei, b_sum,
		    (b_sum - Ei) * (b_sum - Ei) / Ei);

		left_p = right_p;
	}

	if (b_real < CHI_MIN_BUCKETS)
		return 1;

	msg("\t\tCHI^2 RESULT[%u]: %lg vs. %lg  -> %s\n" FNORM,
	    b_real, chi, chi2_read(b_real - 3),
	    chi < chi2_read(b_real - 3) ? FGRN "PASS" : FRED "FAIL");

	t->ed.ok = chi < chi2_read(b_real - 3);

	return 0;
}

void calc_gumbel(struct trace *t, u32 n_samples)
{
	u32 i;
	u32 b_s = 7;
	u32 *marr;
	u32 arr_len = n_samples * FIT_FRAC >> b_s;
	size_t marr_size = arr_len * sizeof(*marr);
	u32 n_distinct = t->max - t->min + 1;
	struct distribution *distr;

	marr = memalign(VEC_SZ, marr_size);
	distr = malloc(n_distinct * sizeof(*distr));

	t->ed.a = 4;
	t->ed.s = t->max - t->min;
	t->ed.m = t->min;

	while (!t->ed.ok) {
		if (n_samples * FIT_FRAC >> b_s < 32)
			break;

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

		t->ed.block_size = 1 << b_s;
		msg("\t\tEVT-%d (%lg): m=%.4lf; s=%.4lf; a=%.3lf  %lg\n",
		    1 << b_s,
		    fit_quality(distr, n_distinct, t->ed.a, t->ed.s, t->ed.m),
		    t->ed.m, t->ed.s, t->ed.a, xceed(t, 0.0001));

		if (chi_2_test(t, arr_len, distr, n_distinct))
			break;

		b_s++;
	}

	if (!t->ed.ok)
		err("Failed to fit distribution\n");
	t->d->distrs_failed |= !t->ed.ok;

	free(marr);
	free(distr);
}


/* The following code tries to fit all three parameters at the same time.
 * It usually gives slightly better fits, but it's also slower. To make it
 * fast make sure computed point-values are reused after move (or retry++).
 * Beware, it may be Frechet specific! (i.e. the correctness conditions)
 */
#if 0
struct move {
	double res;
	s8 gradient[3];
} moves[27] = {
	/*  n                     a   s   m    */
	/*  0 */ { .gradient = { -1, -1, -1 }, },
	/*  1 */ { .gradient = { -1, -1,  0 }, },
	/*  2 */ { .gradient = { -1, -1,  1 }, },
	/*  3 */ { .gradient = { -1,  0, -1 }, },
	/*  4 */ { .gradient = { -1,  0,  0 }, },
	/*  5 */ { .gradient = { -1,  0,  1 }, },
	/*  6 */ { .gradient = { -1,  1, -1 }, },
	/*  7 */ { .gradient = { -1,  1,  0 }, },
	/*  8 */ { .gradient = { -1,  1,  1 }, },
	/*  9 */ { .gradient = {  0, -1, -1 }, },
	/* 10 */ { .gradient = {  0, -1,  0 }, },
	/* 11 */ { .gradient = {  0, -1,  1 }, },
	/* 12 */ { .gradient = {  0,  0, -1 }, },
	/* 13 */ { .gradient = {  0,  0,  0 }, }, /* [13] -> NO_MOVE */
	/* 14 */ { .gradient = {  0,  0,  1 }, },
	/* 15 */ { .gradient = {  0,  1, -1 }, },
	/* 16 */ { .gradient = {  0,  1,  0 }, },
	/* 17 */ { .gradient = {  0,  1,  1 }, },
	/* 18 */ { .gradient = {  1, -1, -1 }, },
	/* 19 */ { .gradient = {  1, -1,  0 }, },
	/* 20 */ { .gradient = {  1, -1,  1 }, },
	/* 21 */ { .gradient = {  1,  0, -1 }, },
	/* 22 */ { .gradient = {  1,  0,  0 }, },
	/* 23 */ { .gradient = {  1,  0,  1 }, },
	/* 24 */ { .gradient = {  1,  1, -1 }, },
	/* 25 */ { .gradient = {  1,  1,  0 }, },
	/* 26 */ { .gradient = {  1,  1,  1 }, },
};

#define NO_MOVE 13

static void shake_all(struct trace *t, struct distribution *distr, u32 n,
		      double a, double s, double m, const u32 max_retry)
{
	u32 i, best_i;
	u32 retry = 0;
	double delta = 1, best;

	while (retry < max_retry) {
		best_i = 30;
		best = -INFINITY;
		for (i = 0; i < 27; i++) {
			if (1.0000001 > a + moves[i].gradient[0] * delta
			    || 0.0000001 > s + moves[i].gradient[1] * delta)
			    || t->min < m + moves[i].gradient[2] * delta
			    || 0 > m + moves[i].gradient[2] * delta)
				continue;

			moves[i].res =
				fit_quality(distr, n,
					    a + moves[i].gradient[0] * delta,
					    s + moves[i].gradient[1] * delta,
					    m + moves[i].gradient[2] * delta);

			if (moves[i].res > best) {
				best = moves[i].res;
				best_i = i;
			}
		}

		assert(best_i < 27);

		dbg("Go   %u(%u) a=%.2lf s=%.2lf m=%.2lf %lf [%le]\n",
		    best_i, retry, a, s, m,
		    moves[best_i].res, best - moves[NO_MOVE].res);

		if (best_i != NO_MOVE) {
			a += moves[best_i].gradient[0] * delta;
			s += moves[best_i].gradient[1] * delta;
			m += moves[best_i].gradient[2] * delta;

			delta = 1;
			retry = 0;
		} else {
			delta /= 2;
			retry++;
		}

		dbg("Went %u(%u) a=%.2lf s=%.2lf m=%.2lf %lf [%le]\n",
		    best_i, retry, a, s, m,
		    moves[best_i].res, best - moves[NO_MOVE].res);
	}

	msg("Shake all would choose: (%lg) m=%lg; s=%lg; a=%lg\n",
	    moves[NO_MOVE].res, m, s, a);

	t->ed.a = a;
	t->ed.s = s;
	t->ed.m = m;
}
#endif
