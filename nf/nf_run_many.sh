#!/bin/bash

HOMANY=1
[ $# -lt 1 ] && echo "gimme result name [and homany]" && exit 1
test $# -ge 2 && HOMANY=$2

IFG=$((2**8))
LEN=96
N_PKTS=$((2**24)) # 16M

# First programm rng seed
nf_set_gener.pl 0 0 4 0
nf_set_gener.pl 0 0 0 0

for i in `seq $HOMANY`
do
    echo -e "\e[32;1mRUNNING $1_$i\e[0m\n"

    tcpdump -i p1p1 -w $1_$i &

    sleep 2

    nf_set_gener.pl $LEN $IFG 2 $N_PKTS
    while [[ ! $(nf_check.pl) =~ fr_cnt_done ]]
          do sleep 1; done
    nf_set_gener.pl 0 0 0 0

    sleep 2

    killall tcpdump
done
