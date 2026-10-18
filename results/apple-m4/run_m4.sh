#!/bin/bash
# M4 data points for the late-September drivers (L1-keep headers, pf3/). Run from the build directory.
cd pf3 || exit 1
./prod_prefetch 27 2000000 5 > ../r3/m4/prod_prefetch.csv 2> ../r3/m4/prod_prefetch.err
../ablation2_arm_L1 16 2000000 9 > ../r3/m4/ablation2_L1_log2n16.csv 2> ../r3/m4/ablation2_L1_log2n16.err
../ablation2_arm_L1 20 2000000 9 > ../r3/m4/ablation2_L1_log2n20.csv 2> ../r3/m4/ablation2_L1_log2n20.err
echo RUN_DONE
