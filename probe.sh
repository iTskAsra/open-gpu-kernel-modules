#!/usr/bin/env bash
# probe.sh — run labeled GPU operations, marking the kernel log around each
set -u
mark(){ echo "GSPMARK $1 op=$2 t=$(date +%s.%N)" | sudo tee /dev/kmsg >/dev/null; }

run_op(){           # run_op <label> <command...>
  local label="$1"; shift
  mark BEGIN "$label"
  "$@" >/dev/null 2>&1
  mark END "$label"
  sleep 0.3         # let async events settle before the next op
}

sudo dmesg -C                       # clear log so the capture starts clean
mark BEGIN session

run_op devicequery   /usr/local/cuda/extras/demo_suite/deviceQuery
run_op bandwidth     /usr/local/cuda/extras/demo_suite/bandwidthTest
run_op vectoradd     ./cuda_ops vectoradd 1048576
run_op alloc_small   ./cuda_ops alloc 4096
run_op alloc_large   ./cuda_ops alloc 1073741824
run_op memcpy_h2d    ./cuda_ops memcpy 16777216
run_op kernel_only   ./cuda_ops kernel 256

mark END session
sudo dmesg > capture_$(date +%Y%m%d_%H%M%S).txt
echo "captured."