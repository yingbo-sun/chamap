#!/bin/bash
# Reproduce all results on this machine (Xeon Gold 6530, SNC2). Needs sudo.
# The /dev/mem steps read (never write) the memmap=32G$98G / memmap=32G$226G reserved DRAM;
# skip them if those ranges are in use or absent (bits 37/38 then stay undetermined).
set -euo pipefail
cd "$(dirname "$0")"

gcc -O2 -Wall -o chamap chamap.c
gcc -O2 -Wall -o lat lat.c

for n in 0 1; do
    echo 1024 | sudo tee /sys/devices/system/node/node$n/hugepages/hugepages-2048kB/nr_hugepages >/dev/null
done
trap 'for n in 0 1; do echo 0 | sudo tee /sys/devices/system/node/node$n/hugepages/hugepages-2048kB/nr_hugepages >/dev/null; done' EXIT

# Which CHA MSR blocks are live (all 64 tick; only 0-31 receive requests).
sudo ./chamap probe > probe.txt

# Training samples: hugepages on each node (first touch from a pinned CPU) + reserved DRAM.
sudo CPU=0 ./chamap sample 100000 n0.txt 32 100
sudo CPU=16 ./chamap sample 100000 n1.txt 32 100
sudo CPU=0 DEVMEM=0x1880000000 MB=32768 SEED=7 ./chamap sample 50000 n0r.txt 32 100
sudo CPU=16 DEVMEM=0x3880000000 MB=32768 SEED=8 ./chamap sample 50000 n1r.txt 32 100
cat n0.txt n0r.txt > n0all.txt
cat n1.txt n1r.txt > n1all.txt
python3 solve.py n0all.txt n1all.txt | tee solve.txt

# Held-out samples: different seeds and CPUs.
sudo CPU=1 SEED=4242 ./chamap sample 20000 v0.txt 32 100
sudo CPU=17 SEED=4343 ./chamap sample 20000 v1.txt 32 100
sudo CPU=2 DEVMEM=0x1880000000 MB=32768 SEED=99 ./chamap sample 20000 v0r.txt 32 100
sudo CPU=18 DEVMEM=0x3880000000 MB=32768 SEED=98 ./chamap sample 20000 v1r.txt 32 100
python3 validate.py v0.txt v1.txt v0r.txt v1r.txt | tee validate.txt

# Core x CHA LLC-hit latency matrix.
sudo ./lat 0 63 > latmat.txt
