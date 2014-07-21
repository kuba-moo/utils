#include <pcap.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <ctype.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <ccan/opt/opt.h>
#include <ccan/list/list.h>
#include <ccan/short_types/short_types.h>

#define FBOLD "\e[1m"
#define FNORM "\e[0m"

#define dbg(fmt...)  if (0) printf(fmt)
#define msg(fmt...)  printf(fmt)
#define pinf(msg)    printf(msg " [pair %llu]\n", pair_no)
#define err(fmt...)  fprintf(stderr, fmt)
#define err_ret(fmt...) ({ err(fmt); 1; })

#define PCAP_CNT_INF		-1
#define PCAP_SNAPLEN_ALL	2048

#define us_to_clk(x) ((x)*1000/8)
#define clk_to_us(x) ((x)*8/1000)

char *g_file_name;
char *g_ifc_name;
int g_search_val;
unsigned long long g_pr_start, g_pr_end;
int g_bucket = 1;
bool g_dump;
bool g_hm;
int g_n_skip;

static struct opt_table opts[] = {
	OPT_WITH_ARG("-i|--interface <ifname>", opt_set_charp, NULL,
		     &g_ifc_name, "live capture on interface <ifname>"),
	OPT_WITH_ARG("-r|--read <file>", opt_set_charp, NULL,
		     &g_file_name, "read packets from pcap <file>"),
	OPT_WITH_ARG("-n|--bucket <val>", opt_set_intval, NULL,
		     &g_bucket, "aggragate <val> results for distribution"),
	OPT_WITH_ARG("-f|--find <val>", opt_set_intval, NULL,
		     &g_search_val, "find samples with delay <val> clocks"),
	OPT_WITH_ARG("-b|--begin <val>", opt_set_ulonglongval_si, NULL,
		     &g_pr_start, "print pairs from no <val>"),
	OPT_WITH_ARG("-e|--end <val>", opt_set_ulonglongval_si, NULL,
		     &g_pr_end, "print pairs to no <val>"),
	OPT_WITHOUT_ARG("-D|--dist", opt_set_bool,
			&g_dump, "dump distributions"),
	OPT_WITHOUT_ARG("-H|--heatmap", opt_set_bool,
			&g_hm, "dump heat map"),
	OPT_WITH_ARG("-s|--skip <s>", opt_set_intval, NULL,
		     &g_n_skip, "skip <n> results after notif"),
	OPT_ENDTABLE
};

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

struct list_head g_pkt_queue[2] = {
	LIST_HEAD_INIT(g_pkt_queue[0]),
	LIST_HEAD_INIT(g_pkt_queue[1])
};

struct enqueued_frame {
	struct list_node node;
	struct result_frame fr;
} __attribute__ ((packed));

#define DIST_SZ (1 << 25)
unsigned long long dist1[DIST_SZ], dist2[DIST_SZ], dist_min[DIST_SZ];

void result_ntoh(struct result *r)
{
	r->rx_ts = ntohl(r->rx_ts);
	r->tx_ts = ntohl(r->tx_ts);
}

static inline int result_get_delta(const u32 rx_ts, const u32 tx_ts)
{
	uint64_t rx = rx_ts;
	uint64_t tx = tx_ts;
	int d;

	/* Fix wrap around - */
	if ((int)tx < 0)
		rx |= 1ULL << 32;
	d = rx - tx;

	return d;
}

static unsigned long long pair_no;

static inline void dist_record(unsigned long long tbl[],
			       unsigned long long val)
{
	if (val > DIST_SZ) {
		msg("Dist overflow: %llu\n", val);
		val = DIST_SZ - 1;
	}
	tbl[val]++;
}

#define MIN 2500
#define M_S (1 << 12)

int hm_max_1, hm_max_2, hm_min_1 = M_S, hm_min_2 = M_S;
unsigned arr[M_S][M_S];

static inline void hm_record(int r1, int r2)
{
	r1 -= MIN;
	r2 -= MIN;
	r1 /= 2;
	r2 /= 2;

	if (r1 < 0 || r2 < 0) {
		msg("HM min underflow by %d %d\n", r1, r2);
		return;
	}

	arr[r1][r2]++;

	if (hm_max_1 < r1)
		hm_max_1 = r1;
	if (hm_max_2 < r2)
		hm_max_2 = r2;
	if (hm_min_1 > r1)
		hm_min_1 = r1;
	if (hm_min_2 > r2)
		hm_min_2 = r2;
}

void hm_dump(void)
{
	int i, j;

	for (i = hm_min_1; i <= hm_max_1; i++) {
		for (j = hm_min_2; j <= hm_max_2; j++)
			printf("%d ", arr[i][j]);
		putchar('\n');
	}
}

void result_process_pair(const struct result *r1, const struct result *r2,
			 u32 tx_ts, bool skip)
{
	static struct result last_r1, last_r2;
	int d1, d2, min, res;

	if (last_r1.tx_ts - tx_ts < 0x1000 && last_r1.tx_ts - tx_ts > 0xe00) {
		pinf("Fixup tx_ts");
		tx_ts ^= 0x1000;
	}

	d1 = result_get_delta(r1->rx_ts, tx_ts);
	d2 = result_get_delta(r2->rx_ts, tx_ts);

	if (d1 > d2) {
		res = d1 - d2;
		min = d2;
	} else {
		res = d2 - d1;
		min = d1;
	}

#define BAD_TRH 4500
#define bad_macro(d)						\
	({							\
		static unsigned long long bad_##d;		\
		if (d > BAD_TRH && !bad_##d)			\
			bad_##d = pair_no;			\
		if (d <= BAD_TRH && bad_##d) {		\
			msg(#d " was bad on pairs [%llu - %llu]\n",	\
			    bad_##d, pair_no - bad_##d);		\
			bad_##d = 0;					\
		}							\
	})
//	bad_macro(d1);
	bad_macro(d2);
//	bad_macro(min);

	if (d1 == g_search_val || d2 == g_search_val)
		msg("found %d at pair %llu\n", g_search_val, pair_no);
	if (pair_no >= g_pr_start && pair_no < g_pr_end)
		msg("%llu T %08x[%04x] R %08x[%04x]%c%08x[%04x]%c  D %d.%03d  %d.%03d us   d %d.%03d\n",
		    pair_no, tx_ts, tx_ts - last_r1.tx_ts,
		    r1->rx_ts, r1->rx_ts - last_r1.rx_ts, r1->tx_ts ? ' ' : 'S',
		    r2->rx_ts, r2->rx_ts - last_r2.rx_ts, r2->tx_ts ? ' ' : 'S',
		    d1*8/1000, d1*8%1000, d2*8/1000, d2*8%1000,
		    res*8/1000, res*8%1000);
	last_r1.rx_ts = r1->rx_ts;
	last_r2.rx_ts = r2->rx_ts;
	last_r1.tx_ts = last_r2.tx_ts = tx_ts;

	//msg("%llu %d %d %d\n", pair_no, d1*8, d2*8, min*8);

	if (skip)
		return;

	if (g_dump) {
		dist_record(dist1, d1);
		dist_record(dist2, d2);
		dist_record(dist_min, min);
	}
	if (g_hm)
		hm_record(d1, d2);
}

static inline bool is_time_backward(s64 ts, u32 *__mem)
{
	s64 mem = *__mem;
	*__mem = ts;

	/* Timestamp zeroed - probably cond_resched() notif */
	if (!ts)
		return false;
	/* Typical case - time flows ok */
	if (ts > mem && (ts - 0xFFFF < mem || !mem))
		return false;
	/* Wrap-around */
	if ((int)mem < 0 && ts < 0xFFFF)
		return false;

	//msg("Nope %llu %llu %llx %llx     %lld\n", ts, mem, ts, mem, ts - mem);

	return true;
}

void packet_cb(u_char *args, const struct pcap_pkthdr *header,
	       const u_char *packet)
{
	static int skip_frames;
	u8 src, other;
	struct result_frame *fr = (void *)packet, *dut1, *dut2;
	struct enqueued_frame *ofr;
	int i;

	if (header->len != sizeof(*fr)) {
		err("Wrong sized packet: %d!\n", header->len);
		return;
	}

	src = fr->key & 1;
	other = src ^ 1;

	/* If other DUT's result isn't in yet, enqueue packet and wait. */
	if (list_empty(&g_pkt_queue[other])) {
		struct enqueued_frame *copy = malloc(sizeof(*copy));

		memcpy(&copy->fr, packet, header->len);

		if (!list_empty(&g_pkt_queue[src]))
			msg("Multi enqueue %llu\n", pair_no/128);
		list_add_tail(&g_pkt_queue[src], &copy->node);

		return;
	}

	ofr = list_pop(&g_pkt_queue[other], struct enqueued_frame, node);

	dut1 = src ? fr : &ofr->fr;
	dut2 = other ? fr : &ofr->fr;
	if (dut1->key != 0x55 || dut2->key != 0xaa)
		pinf("Keys wrong!");
	for (i = 0; i < FR_N_RES; i++) {
		u32 tx_ts;
		const bool is_notif = !dut1->r[i].tx_ts || !dut2->r[i].tx_ts;

		result_ntoh(&dut1->r[i]);
		result_ntoh(&dut2->r[i]);

		tx_ts = dut1->r[i].tx_ts ?: dut2->r[i].tx_ts;
		if (is_notif) {
			if (skip_frames)
				pinf("Multiple skip frames!");
			skip_frames = g_n_skip;

			if (!dut1->r[i].tx_ts && !dut2->r[i].tx_ts) {
				pinf("Double skip!");
				skip_frames = skip_frames ?: 1;
			}
			/*
			msg("skip %d %d [pair %llu]\n",
			!!dut1->r[i].tx_ts, !!dut2->r[i].tx_ts, pair_no);*/
		}

		if (dut1->r[i].tx_ts != dut2->r[i].tx_ts && !is_notif) {
			pinf("Frame tx ts mismatch");
			goto cb_out;
		}
		/*
		if (is_time_backward(dut1->r[i].tx_ts, &last_tx_ts) ||
		    is_time_backward(dut1->r[i].rx_ts, &last_rx_ts1) ||
		    is_time_backward(dut2->r[i].rx_ts, &last_rx_ts2))
			msg("Time runs backward [pair %llu]\n", pair_no);
		*/
		result_process_pair(&dut1->r[i], &dut2->r[i],
				    tx_ts, !!skip_frames);

		if (skip_frames)
			skip_frames--;

		pair_no++;
	}

cb_out:
	free(ofr);
}

void dist_dump(unsigned long long *t1, unsigned long long *t2,
	       unsigned long long *t3)
{
	int i, j;

	for (i = 0; i < DIST_SZ; i += g_bucket) {
		unsigned long long v1 = 0, v2 = 0, v3 = 0;

		for (j = 0; j < g_bucket; j++) {
			v1 += t1[i+j];
			v2 += t2[i+j];
			v3 += t3[i+j];
		}

		if (v1 || v2 || v3)
			msg("%d %llu %llu %llu\n", i*8, v1, v2, v3);
	}
}

int main(int argc, char **argv)
{
	char errbuf[PCAP_ERRBUF_SIZE];
	pcap_t *pcap_src = NULL;

	opt_register_table(opts, NULL);

	if (!opt_parse(&argc, argv, opt_log_stderr))
		return 1;

	if (!g_ifc_name && !g_file_name)
		return err_ret("No packet source specified\n");
	if (g_ifc_name && g_file_name)
		return err_ret("Both file and interface source specified\n");

	if (g_ifc_name)
		pcap_src = pcap_open_live(g_ifc_name, PCAP_SNAPLEN_ALL,
					  1, -1, errbuf);
	else if (g_file_name)
		pcap_src = pcap_open_offline(g_file_name, errbuf);

	if (!pcap_src) {
		fprintf(stderr, "Couldn't open packet source: %s\n", errbuf);
		return 1;
	}

	pcap_loop(pcap_src, PCAP_CNT_INF, packet_cb, NULL);

	pcap_close(pcap_src);

	if (g_dump)
		dist_dump(dist1, dist2, dist_min);

	if (g_hm)
		hm_dump();

	return 0;
}
