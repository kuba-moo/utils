#!/usr/bin/perl -w

use strict;
use NF::RegAccess;
use reg_defines_rtt_probe;

my $marker = nf_regread('nf2c0', STATS_MARKER_REG ());

if ($marker != 0xaabbccdd) {
    print "\e[31mMarker not found! ", $marker, "\e[0m\n";
    exit 1;
}

my $st_hdr_drop = nf_regread('nf2c0', STATS_CNT_DROP_HDR_REG ());
my $st_samples = nf_regread('nf2c0', STATS_CNT_SAMPLES_REG ());
my $st_stats = nf_regread('nf2c0', STATS_CNT_STAT_PKTS_REG ());

print "Stats:\n";
print "   hdr drop: ", $st_hdr_drop, " samples: ", $st_samples, " stats: ", $st_stats, "\n";

#############

my $gen_cnt = nf_regread('nf2c0', GENER_CNT_PKTS_REG ());
my $gen_ctrl = nf_regread('nf2c0', GENER_CTRL_REG ());
my $gen_len = nf_regread('nf2c0', GENER_LEN_REG ());
my $gen_ifg = nf_regread('nf2c0', GENER_IFG_REG ());
my $gen_seed = nf_regread('nf2c0', GENER_SEED_REG ());
my $gen_fr_cnt = nf_regread('nf2c0', GENER_FRAME_CNT_REG ());
my $gen_rng_lo = nf_regread('nf2c0', GENER_RNG_OUT_LO_REG ());
my $gen_rng_hi = nf_regread('nf2c0', GENER_RNG_OUT_HI_REG ());
my $gen_status = nf_regread('nf2c0', GENER_STATUS_REG ());

print "Generator:\n";
printf("   cnt: %08x ctrl: %08x len: %08x ifg: %08x\n   status: [s:%02x", $gen_cnt, $gen_ctrl, $gen_len, $gen_ifg, $gen_status & ((1 << 7) - 1));
$gen_status & 0x80 && print " out_rdy";
$gen_status & 0x100 && print " in_fifo_empty";
$gen_status & 0x200 && print " out_wr_int";
$gen_status & 0x400 && print " pkt_upcnt";
$gen_status & 0x800 && print " fr_cnt_done";
printf(" cnt:%04x]\n", $gen_status >> 15);
printf("   fr_cnt: %08x seed: %08x rng: %08x%08x\n", $gen_fr_cnt, $gen_seed, $gen_rng_lo, $gen_rng_hi);
