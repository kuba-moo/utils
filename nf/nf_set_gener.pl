#!/usr/bin/perl -w

use strict;
use NF::RegAccess;
use reg_defines_rtt_probe;

if (scalar(@ARGV) != 4) {
    print "\e[31mUsage:\e[0m ./cmd len ifg ctrl n_fr\n";
    exit 1;
}

my $marker = nf_regread('nf2c0', STATS_MARKER_REG ());

if ($marker != 0xaabbccdd) {
    printf("\e[31mMarker not found! %x\e[0m\n", $marker);
    exit 1;
}

nf_regwrite('nf2c0', GENER_SEED_REG (), 0x12345678);
nf_regwrite('nf2c0', GENER_FRAME_CNT_REG (), $ARGV[3]);
nf_regwrite('nf2c0', GENER_LEN_REG (), $ARGV[0]);
nf_regwrite('nf2c0', GENER_IFG_REG (), $ARGV[1]);
nf_regwrite('nf2c0', GENER_CTRL_REG (), $ARGV[2]);

my $gen_cnt = nf_regread('nf2c0', GENER_CNT_PKTS_REG ());
my $gen_ctrl = nf_regread('nf2c0', GENER_CTRL_REG ());
my $gen_len = nf_regread('nf2c0', GENER_LEN_REG ());
my $gen_ifg = nf_regread('nf2c0', GENER_IFG_REG ());

printf("   cnt: %08x ctrl: %08x len: %08x ifg: %08x\n", $gen_cnt, $gen_ctrl, $gen_len, $gen_ifg);
