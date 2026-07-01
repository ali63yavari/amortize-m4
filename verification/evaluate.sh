#!/bin/bash

TIMESTAMP=$(date +"%H%M_%d%m%Y")

# Combine the timestamp into your final string
OUT_DIR="${TIMESTAMP}_verification"
echo $OUT_DIR

python3 /work/run_step3_ironmask_pini_all.py \
  --ironmask /work/IronMask/src/ironmask \
  --gadgets-c /work/gadgets/gadgets.c \
  --out-dir /work/results/$OUT_DIR \
  --max-order 5 \
  --jobs 4 \
  --timeout 3600 \
  --include-pini \
  "$@"
