#!/bin/bash
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
firmware=$(dirname -- "$here")
client="$firmware/../mioty_at_client_c/src"
out=$(mktemp -d /tmp/mioty-v3-tests.XXXXXX)
trap 'rm -rf "$out"' EXIT
flags=(-g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer)
c++ -std=c++14 -Wall -Wextra -Werror "${flags[@]}" -I"$firmware" -I"$client" "$here/test_protocol.cpp" -o "$out/protocol"
ASAN_OPTIONS=detect_leaks=0 "$out/protocol"
for source in "$client/miotyAtClient.c" "$client/data_tools/string_tools.c" "$client/data_tools/char_tools.c"; do
  cc -std=c11 -w "${flags[@]}" -I"$client" -c "$source" -o "$out/$(basename -- "$source").o"
done
c++ -std=c++14 -Wall -Wextra -Werror "${flags[@]}" -I"$here/fakes" -I"$firmware" -I"$client" \
  "$firmware/AtTestRunner.cpp" "$here/test_runner.cpp" "$out"/*.o -o "$out/runner"
for width in 1 7 30; do
  ASAN_OPTIONS=detect_leaks=0 "$out/runner" "$width"
done
ASAN_OPTIONS=detect_leaks=0 "$out/runner" 7 missing
ASAN_OPTIONS=detect_leaks=0 "$out/runner" 30 overflow
