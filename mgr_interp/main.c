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

/* Bigger tool which uses libpcap to read statistics generated by NetFPGA
 * from file or directly from the capturing interface and does calculations.
 */

#include "mgr_interp.h"

#include <dirent.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <ccan/opt/opt.h>
#include <ccan/tal/tal.h>

struct cmdline_args args = {
	.res_dir = "./",
};

static struct opt_table opts[] = {
	OPT_WITH_ARG("-p|--pfx <prefix>", opt_set_charp, NULL,
		     &args.res_pfx, "read files from result dir with names <prefix>*"),
	OPT_WITH_ARG("-d|--res-dir <path>", opt_set_charp, NULL,
		     &args.res_dir, "look for result files in <path>, default ./"),
	OPT_WITH_ARG("-i|--ifg <clks>", opt_set_intval, NULL,
		     &args.ifg, "expected inter frame gap"),
	OPT_WITH_ARG("-s|--skip-notif <n>", opt_set_uintval, NULL,
		     &args.skip_notif, "skip <n> results when notif seen"),
	OPT_WITH_ARG("-b|--skip-begin <n>", opt_set_uintval, NULL,
		     &args.skip_begin, "skip <n> at the beginning"),
	OPT_WITH_ARG("-D|--distribution <dir>", opt_set_charp, NULL,
		     &args.distr, "dump distributions to given directory"),
	OPT_WITH_ARG("-H|--heatmap <dir>", opt_set_charp, NULL,
		     &args.hm, "dump heatmaps to given directory"),
	OPT_WITH_ARG("-S|--stats <dir>", opt_set_charp, NULL,
		     &args.stats, "write mean,stdev,correlation to given directory"),
	OPT_WITH_ARG("-n|--aggregate <n>", opt_set_intval, NULL,
		     &args.aggr, "aggregation for simple statistics (bucket size)"),
	OPT_WITHOUT_ARG("-q|--quiet", opt_set_bool,
			&args.quiet, "suppress text output"),
	OPT_ENDTABLE
};

typedef int (*delay2file_fn)(struct delay *d, FILE *f);

static int make_distr(struct delay *d, FILE *f)
{
	u32 i;
	u32 sums[3];
	u32 val = d->t[2].min; /* t2 is min(t0,t1) so it has the global min */
	u32 val_end = d->t[0].max > d->t[1].max ? d->t[0].max : d->t[1].max;
	struct distribution *di[3], *di_end[3];

	for (i = 0; i < 3; i++) {
		di[i] = d->t[i].distr;
		di_end[i] = d->t[i].distr + tal_count(d->t[i].distr);
	}

	while (val <= val_end) {
		memset(sums, 0, sizeof(sums));

		for (i = 0; i < 3; i++)
			while (di[i] < di_end[i] &&
			       di[i]->val < val + args.aggr)
				sums[i] += (di[i]++)->cnt;

		if (sums[0] || sums[1] ||sums[2])
			fprintf(f, "%d %u %u %u\n", val,
				sums[0], sums[1], sums[2]);

		val += args.aggr;
	}

	return 0;
}

#define aggr(_t_) ((d->t[_t_].samples[i] - d->t[_t_].min) / args.aggr)
static int make_hm(struct delay *d, FILE *f)
{
	u32 i, j;
	u32 **hm_table;
	u32 dim[2];

	dim[0] = 1 + (d->t[0].max - d->t[0].min) / args.aggr;
	dim[1] = 1 + (d->t[1].max - d->t[1].min) / args.aggr;

	hm_table = calloc(dim[0], sizeof(*hm_table));
	for (i = 0; i < dim[0]; i++)
		hm_table[i] = calloc(dim[1], sizeof(**hm_table));

	for (i = 0; i < d->n_samples; i++)
		hm_table[aggr(0)][aggr(1)]++;

	for (i = 0; i < dim[0]; i++) {
		for (j = 0; j < dim[1]; j++)
			fprintf(f, "%d ", hm_table[i][j]);
		fputc('\n', f);
	}

	for (i = 0; i < dim[0]; i++)
		free(hm_table[i]);
	free(hm_table);

	return 0;
}

static int make_stats(struct delay *d, FILE *f)
{
	struct trace *t;

	for_each_trace(d, t) {
		fprintf(f, "%lf %lf", t->mean, t->stdev);
		if (t->ed.ok)
			fprintf(f, "   %lf %lf %lf",
				t->ed.a, t->ed.s, t->ed.m);
		fputc('\n', f);
	}

	fprintf(f, "%lf\n", d->corr);

	return 0;
}

#undef aggr
#undef deggr

static inline int make_delay_file(struct delay *d, delay2file_fn make_single)
{
	int ret;
	FILE *f;

	f = fopen(d->fname, "w");
	if (!f)
		return perr_ret("Opening distr file to write failed");

	ret = make_single(d, f);

	fclose(f);

	return ret;
}

static int make_per_delay(const char *dir, struct delay_bank *db,
			  delay2file_fn make_single)
{
	int i, res;
	char *cwd;

	res = mkdir(dir, 0777);
	if (res && errno != EEXIST)
		return perr_ret("Could not create distribution directory\n");

	cwd = get_current_dir_name();
	if (!cwd)
		return perr_ret("Could not get current dir");
	if (chdir(dir))
		return perr_ret("Could not go to the result dir");

	for (i = 0; i < db->n; i++)
		make_delay_file(db->bank[i], make_single);

	chdir(cwd);
	free(cwd);

	return 0;
}

static void calc_all_stats(struct delay *d)
{
	struct trace *t;
	int i;

	for_each_trace_i(d, t, i) {
		calc_distr(t);
		calc_mean(t, d->n_samples);
		calc_stdev(t, d->n_samples);
		calc_gumbel(t, d->n_samples);

		msg("\tTrace %d: min %u max %u mean %lf stdev %lf\n",
		    i, t->min, t->max, t->mean, t->stdev);
	}
	calc_corr(d);
	msg("\tCorrelation: %lf\n", d->corr);
}

static struct delay_bank *open_many(const char *dname, const char *pfx)
{
	DIR *dir;
	struct dirent *ent;
	int pfx_len = pfx ? strlen(pfx) : 0;
	char *cwd;
	struct delay_bank *db;
	struct delay *d;
	u32 full_distr = 0;

	cwd = get_current_dir_name();
	if (!cwd)
		return perr_nret("Could not get current dir");
	if (chdir(dname))
		return perr_nret("Could not go to the result dir");

	dir = opendir(".");
	if (!dir)
		return perr_nret("Could not open the result dir");

	db = talz(NULL, struct delay_bank);
	db->min_samples = -1;

	while ((ent = readdir(dir))) {
		if (pfx && strncmp(ent->d_name, pfx, pfx_len))
			continue;

		if (db->bank)
			tal_resize(&db->bank, ++db->n);
		else
			db->bank = tal_arr(db, struct delay *, ++db->n);

		d = read_delay(ent->d_name);
		if (!d) {
			tal_free(db);
			db = NULL;
			goto out;
		}

		if (db->min_samples > d->n_samples)
			db->min_samples = d->n_samples;

		calc_all_stats(d);
		if (!d->distrs_failed)
			full_distr++;

		tal_steal(db->bank, d);
		db->bank[db->n - 1] = d;
	}

	msg("Read %d files [at least %u samples][%u full distrs]\n",
	    db->n, db->min_samples, full_distr);

out:
	closedir(dir);

	chdir(cwd);
	free(cwd);

	return db;
}

int main(int argc, char **argv)
{
	struct delay_bank *db;

	opt_register_table(opts, NULL);

	if (!opt_parse(&argc, argv, opt_log_stderr))
		return 1;

	opt_free_table();

	if (!args.ifg)
		err("Consider setting ifg to improve parsing accuracy\n");

	db = open_many(args.res_dir, args.res_pfx);
	if (!db)
		return 1;

	if (args.distr)
		make_per_delay(args.distr, db, make_distr);
	if (args.hm)
		make_per_delay(args.hm, db, make_hm);
	if (args.stats)
		make_per_delay(args.stats, db, make_stats);

	tal_free(db);

	tal_cleanup();

	return 0;
}
