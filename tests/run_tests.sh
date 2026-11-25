#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

CXX_WARN="-Wall -Wextra -Wno-unused-function -Wno-unused-parameter"
ASAN_FLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer"

echo "=== 1. C++ ASan/UBSan verification matrix ==="
for cfg in \
  "fast:" \
  "fast32:-m32 -D_FILE_OFFSET_BITS=64" \
  "fast_no_lazy:-DGIF_LAZY_RESET=0" \
  "balanced:-DGIF_PROFILE_BALANCED" \
  "minimal:-DGIF_PROFILE_MINIMAL" \
  "minimal240:-DGIF_PROFILE_MINIMAL -DMAX_WIDTH=240" \
  "coop:-DGIF_PROFILE_COOPERATIVE" \
  "minimal_coop:-DGIF_PROFILE_MINIMAL -DGIF_ENABLE_COOPERATIVE=1" \
  "dict16_full:-DGIF_DICT_MODE=16 -DGIF_FILE_BUF_SIZE=4096" \
  "dict12_full:-DGIF_DICT_MODE=12 -DGIF_FILE_BUF_SIZE=4096" \
  "dict12_lzw264:-DGIF_PROFILE_MINIMAL -DGIF_LZW_BUF_SIZE=264" \
  "transient:-DGIF_PROFILE_TRANSIENT" \
  "transient32:-m32 -D_FILE_OFFSET_BITS=64 -DGIF_PROFILE_TRANSIENT" \
  "transient_coop:-DGIF_PROFILE_TRANSIENT -DGIF_ENABLE_COOPERATIVE=1" \
  "transient_dict0:-DGIF_TRANSIENT_WORK=1 -DGIF_DICT_MODE=0 -DGIF_LAZY_RESET=0" \
  "transient_dict12:-DGIF_TRANSIENT_WORK=1 -DGIF_DICT_MODE=12 -DGIF_FILE_BUF_SIZE=768 -DGIF_RGB565_ONLY=1" \
  "transient_4k:-DGIF_PROFILE_TRANSIENT -DGIF_FILE_BUF_SIZE=4096"
do
  name="${cfg%%:*}"
  flags="${cfg#*:}"
  echo "--- Testing profile: $name ($flags) ---"
  g++ -std=c++17 $ASAN_FLAGS $CXX_WARN $flags test_runner.cpp -o "/tmp/agif_test_$name"
  ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "/tmp/agif_test_$name" sizeof
  if [[ "$name" == "transient" || "$name" == "transient_coop" ]]; then
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "/tmp/agif_test_$name" verify corpus golden_hashes.txt
  else
    ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "/tmp/agif_test_$name" verify corpus golden_hashes.txt
  fi
done

echo "=== 2. Pure C compilation check (C API) ==="
gcc -std=c99 -O2 -Wall -Wextra -Wno-unused-function -Wno-unused-parameter -D__LINUX__ -DGIF_PROFILE_MINIMAL -DGIF_ENABLE_COOPERATIVE=1 -I../src -include AnimatedGIF.h -x c -c ../src/gif.inl -o /tmp/agif_c_check.o
gcc -std=c99 -O2 -Wall -Wextra -Wno-unused-function -Wno-unused-parameter -D__LINUX__ -DGIF_PROFILE_TRANSIENT -DGIF_ENABLE_COOPERATIVE=1 -I../src -include AnimatedGIF.h -x c -c ../src/gif.inl -o /tmp/agif_c_transient_check.o
echo "Pure C (C99) compilation OK"

echo "=== 3. Fuzzing (ASan + UBSan) ==="
g++ -std=c++17 $ASAN_FLAGS -m32 -D_FILE_OFFSET_BITS=64 -DGIF_PROFILE_MINIMAL -DGIF_ENABLE_COOPERATIVE=1 fuzz.cpp -o /tmp/agif_fuzz_min
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 /tmp/agif_fuzz_min corpus "${1:-50000}" -1

g++ -std=c++17 $ASAN_FLAGS -DGIF_PROFILE_FAST fuzz.cpp -o /tmp/agif_fuzz_fast
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 /tmp/agif_fuzz_fast corpus "${1:-25000}" -1

g++ -std=c++17 $ASAN_FLAGS -m32 -D_FILE_OFFSET_BITS=64 -DGIF_PROFILE_TRANSIENT -DGIF_ENABLE_COOPERATIVE=1 fuzz.cpp -o /tmp/agif_fuzz_transient
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 /tmp/agif_fuzz_transient corpus "${1:-25000}" -1

echo "ALL TESTS PASSED"
