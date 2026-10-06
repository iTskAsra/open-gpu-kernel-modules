#!/usr/bin/env bash
# segment.sh capture.txt — per-operation RPC tally
awk '
  /GSPMARK BEGIN op=/ { match($0,/op=[^ ]+/); op=substr($0,RSTART+3,RLENGTH-3); next }
  /GSPMARK END/        { op=""; next }
  /GSPTRACE/ && op!="" {
     match($0,/func=[0-9]+/); f=substr($0,RSTART+5,RLENGTH-5);
     key=op"\t"f; c[key]++
  }
  END{ for(k in c) print c[k]"\t"k }
' "$1" | sort | column -t