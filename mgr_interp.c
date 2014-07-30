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

#define _GNU_SOURCE 1

#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <pcap.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <ccan/opt/opt.h>
#include <ccan/tal/tal.h>
#include <ccan/list/list.h>
#include <ccan/short_types/short_types.h>

#define FBOLD "\e[1m"
#define FNORM "\e[0m"

#define dbg(fmt...)  if (0) printf(fmt)
#define msg(fmt...)  ({ if (g_msg) printf(fmt); })
#define pinf(msg)    ({ if (g_msg) printf(msg " [pair %u]\n", sc->d->n_samples); })
#define err(fmt...)  fprintf(stderr, fmt)
#define err_ret(fmt...) ({ err(fmt); 1; })
#define err_nret(fmt...) ({ err(fmt); NULL; })
#define perr_ret(msg) ({ perror(msg); 1; })
#define perr_nret(msg) ({ perror(msg); NULL; })

#define PCAP_CNT_INF		-1
#define PCAP_SNAPLEN_ALL	2048

#define us_to_clk(x) ((x)*1000/8)
#define clk_to_us(x) ((x)*8/1000)

bool g_msg = true;

struct {
	int ifg;
	char *res_pfx;
	char *res_dir;
} args = {
	.res_dir = "./",
};

static struct opt_table opts[] = {
	OPT_WITH_ARG("-p|--pfx <prefix>", opt_set_charp, NULL,
		     &args.res_pfx, "read files from result dir with names <prefix>*"),
	OPT_WITH_ARG("-d|--res-dir <path>", opt_set_charp, NULL,
		     &args.res_dir, "look for result files in <path>, default ./"),
	OPT_WITH_ARG("-i|--ifg <clks>", opt_set_intval, NULL,
		     &args.ifg, "expected inter frame gap"),
	OPT_WITHOUT_ARG("-q|--quiet", opt_set_invbool,
			&g_msg, "suppress text output"),
	OPT_ENDTABLE
};

/* Actual packet structures, note that all fields are in network order. */
struct result {
	uint32_t rx_ts;
	uint32_t tx_ts;
};

#define FR_N_RES 128

struct result_frame {
	struct result r[FR_N_RES];
	uint8_t key;
	uint64_t ts;
} __attribute__ ((packed));

/* Wait queue to match stats from different DUTs */
struct list_head g_pkt_queue[2] = {
	LIST_HEAD_INIT(g_pkt_queue[0]),
	LIST_HEAD_INIT(g_pkt_queue[1])
};

struct enqueued_frame {
	struct list_node node;
	struct result_frame fr;
} __attribute__ ((packed));

/* Program samples, struct result is translated to this one. */
struct sample {
	u64 tx_ts;    /* True tx time stamp. */
	u64 rx_ts[2]; /* RX time stamps for machines. */
};

struct delay;
struct sample_context {
	pcap_t *pcap;

	bool is_first;
	bool is_notif;
	struct sample c, p; /* current and previos sample. */

	struct delay *d;
};

static void sc_reset(struct sample_context *sc, struct delay *d, pcap_t *pcap)
{
	memset(sc, 0, sizeof(*sc));

	sc->d = d;
	sc->pcap = pcap;
	sc->is_first = true;
}

static inline void sc_next(struct sample_context *sc)
{
	sc->p = sc->c;

	sc->is_notif = sc->is_first = false;
}

struct delay {
	int n_samples;
	int n_notifs;

	int trace_size_;
	u32 *traces[3];
};

struct delay_bank {
	struct delay **bank;
};

void delay_trace_grow(struct delay *d)
{
	int i;

	if (!d->trace_size_) {
		d->trace_size_ = 2048;
		for (i = 0; i < 3; i++)
			d->traces[i] = tal_arr(d, u32, d->trace_size_);
	} else {
		d->trace_size_ *= 2;
		for (i = 0; i < 3; i++)
			tal_resize(&d->traces[i], d->trace_size_);
	}
}

static inline void delay_push(struct delay *d, u32 t1, u32 t2, u32 t3)
{
	assert(d->trace_size_ >= d->n_samples);

	if (unlikely(d->trace_size_ == d->n_samples))
		delay_trace_grow(d);

	d->traces[0][d->n_samples] = t1;
	d->traces[1][d->n_samples] = t2;
	d->traces[2][d->n_samples] = t3;
	d->n_samples++;
}

static inline void sc_load_res(struct sample_context *sc,
			       const struct result r1, const struct result r2)
{
	sc->is_notif = !r1.tx_ts || !r2.tx_ts;

	sc->c.tx_ts = ntohl(r1.tx_ts) ?: ntohl(r2.tx_ts);
	sc->c.rx_ts[0] = ntohl(r1.rx_ts);
	sc->c.rx_ts[1] = ntohl(r2.rx_ts);
}

static inline int sc_check_double_skip(const struct sample_context *sc)
{
	if (likely(!sc->is_notif))
		return 0;

	if (!sc->c.tx_ts) {
		if (sc->is_first)
			pinf("\tDouble skip, ignoring first sample");
		else
			err("\tFIXME: Double skip, ignoring sample\n");

		return 1;
	}

	return 0;
}

static inline void unwrap_time_(u64 *prev, u64 *curr)
{
	u64 prev_high_bit = *prev & (1U << 31);
	u64 curr_high_bit = *curr & (1U << 31);

	*curr |= (prev_high_bit & ~curr_high_bit) << 1;
	*prev &= 0xFFFFFFFF;
}

static inline void sc_unwrap_time(struct sample_context *sc)
{
	unwrap_time_(&sc->p.tx_ts, &sc->c.tx_ts);
	unwrap_time_(&sc->p.rx_ts[0], &sc->c.rx_ts[0]);
	unwrap_time_(&sc->p.rx_ts[1], &sc->c.rx_ts[1]);
}

static inline void sc_check_ifg(struct sample_context *sc)
{
	s64 expected_ts_diff;

	if (!args.ifg || sc->is_first)
		return;

	expected_ts_diff = sc->p.tx_ts + args.ifg - sc->c.tx_ts;

	if (unlikely(expected_ts_diff < -0x900 &&
		     expected_ts_diff > -0x1100)) {
		pinf("\tFixup tx_ts");
		sc->c.tx_ts ^= 0x1000;
		expected_ts_diff = sc->p.tx_ts + args.ifg - sc->c.tx_ts;
	}

	if (unlikely(expected_ts_diff < -0x80 ||
		     expected_ts_diff >  0x80)) {
		err("Broken tx_ts\n");
	}
}

static inline void sc_save_deltas(struct sample_context *sc)
{
	u32 d1, d2, min;

	d1 = sc->c.rx_ts[0] - sc->c.tx_ts;
	d2 = sc->c.rx_ts[1] - sc->c.tx_ts;

	min = d1 < d2 ? d1 : d2;

	delay_push(sc->d, d1, d2, min);
}

void packet_cb(u_char *args, const struct pcap_pkthdr *header,
	       const u_char *packet)
{
	struct sample_context *sc = (void *)args;
	struct delay *d = sc->d;
	struct result_frame *fr = (void *)packet, *dut1, *dut2;
	struct enqueued_frame *ofr;
	u8 src, other;
	int i;

	if (header->len != sizeof(*fr)) {
		err("Wrong sized packet: %d!\n", header->len);
		pcap_breakloop(sc->pcap);
		return;
	}

	src = fr->key & 1;
	other = src ^ 1;

	/* If other DUT's result isn't in yet, enqueue packet and wait. */
	if (list_empty(&g_pkt_queue[other])) {
		struct enqueued_frame *copy = malloc(sizeof(*copy));

		memcpy(&copy->fr, packet, header->len);

		if (!list_empty(&g_pkt_queue[src]))
			msg("Multi enqueue %u\n", d->n_samples/128);
		list_add_tail(&g_pkt_queue[src], &copy->node);

		return;
	}

	ofr = list_pop(&g_pkt_queue[other], struct enqueued_frame, node);

	dut1 = src ? fr : &ofr->fr;
	dut2 = other ? fr : &ofr->fr;
	if (dut1->key != 0x55 || dut2->key != 0xaa) {
		pinf("Keys wrong!");
		pcap_breakloop(sc->pcap);
		goto cb_out;
	}

	for (i = 0; i < FR_N_RES; i++) {
		sc_load_res(sc, dut1->r[i], dut2->r[i]);

		if (dut1->r[i].tx_ts != dut2->r[i].tx_ts && !sc->is_notif) {
			pinf("Frame tx ts mismatch");
			pcap_breakloop(sc->pcap);
			goto cb_out;
		}

		if (sc->is_notif)
			d->n_notifs++;

		if (sc_check_double_skip(sc))
			continue;

		sc_unwrap_time(sc);

		sc_check_ifg(sc);

		sc_save_deltas(sc);

		sc_next(sc);
	}

cb_out:
	free(ofr);
}

struct delay *read_delay(const char *fname)
{
	int res;
	struct delay *d;
	pcap_t *pcap_src = NULL;
	char errbuf[PCAP_ERRBUF_SIZE];
	struct sample_context sc;

	msg("Loading file %s\n", fname);

	pcap_src = pcap_open_offline(fname, errbuf);
	if (!pcap_src)
		return err_nret("Could not load packets: %s\n", errbuf);

	d = talz(NULL, struct delay);
	sc_reset(&sc, d, pcap_src);

	res = pcap_loop(pcap_src, PCAP_CNT_INF, packet_cb, (void *)&sc);
	if (res) {
		/* Print pcap msg if break was due to internal pcap error. */
		if (res == -1)
			pcap_perror(pcap_src, "Error while reading packets");
		tal_free(d);
		d = NULL;
	}

	msg("\tLoaded %d samples, %d notifs\n", d->n_samples, d->n_notifs);
	pcap_close(pcap_src);

	return d;
}

struct delay_bank *open_many(const char *dir, const char *pfx)
{
	DIR *d;
	struct dirent *ent;
	int pfx_len = strlen(pfx);
	char *cwd;
	int n_res = 0;
	struct delay_bank *rb;

	cwd = get_current_dir_name();
	if (!cwd)
		return perr_nret("Could not get current dir");
	if (chdir(dir))
		return perr_nret("Could not go to the result dir");

	d = opendir(".");
	if (!d)
		return perr_nret("Could not open the result dir");

	rb = talz(NULL, struct delay_bank);

	while ((ent = readdir(d))) {
		if (strncmp(ent->d_name, pfx, pfx_len))
			continue;

		if (rb->bank)
			tal_resize(&rb->bank, ++n_res);
		else
			rb->bank = tal_arr(rb, struct delay *, ++n_res);

		rb->bank[n_res - 1] = read_delay(ent->d_name);
		if (!rb->bank[n_res - 1]) {
			tal_free(rb);
			rb = NULL;
			break;
		}
		tal_steal(rb->bank, rb->bank[n_res - 1]);
	}

	closedir(d);

	chdir(cwd);
	free(cwd);

	return rb;
}

int main(int argc, char **argv)
{
	struct delay_bank *db;

	opt_register_table(opts, NULL);

	if (!opt_parse(&argc, argv, opt_log_stderr))
		return 1;

	opt_free_table();

	if (!args.res_pfx)
		return err_ret("Please specify result file name prefix\n");

	db = open_many(args.res_dir, args.res_pfx);
	if (!db)
		return 1;

	tal_free(db);

	tal_cleanup();

	return 0;
}
