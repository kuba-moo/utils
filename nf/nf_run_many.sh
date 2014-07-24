#!/bin/bash

[ -z "$RES" ] && RES="nf_result"
[ -z "$HOMANY" ] && HOMANY=1
[ -z "$IFG" ] && IFG=$((2**11))
[ -z "$LEN" ] && LEN=1024
[ -z "$N_PKTS" ] && N_PKTS=$((2**20)) # 1M

# Say what we got
echo RES: $RES HOMANY: $HOMANY IFG: $IFG LEN: $LEN N_PKTS: $N_PKTS

# First programm rng seed
nf_set_gener.pl 0 0 4 0
nf_set_gener.pl 0 0 0 0

for i in `seq $HOMANY`
do
    echo -e "\e[32;1mRUNNING ${RES}_$i\e[0m\n"

    tcpdump -i p1p1 -w ${RES}_$i &

    sleep 2

    nf_set_gener.pl $LEN $IFG 2 $N_PKTS
    while [[ ! $(nf_check.pl) =~ fr_cnt_done ]]
          do sleep 1; done
    nf_set_gener.pl 0 0 0 0

    sleep 2

    killall tcpdump

    sleep 1
done
