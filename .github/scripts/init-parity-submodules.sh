#!/usr/bin/env bash
# Initialize build dependencies without the unpublished test-run gitlink.
# The parity jobs launch the standalone SQL harness, not test-run.py.
set -euo pipefail

mapfile -t required_submodules < <(
  git ls-files --stage |
    awk '$1 == 160000 && $4 != "test-run" && $4 != "checkpatch" &&
         $4 != "third_party/luarocks" {print $4}'
)
if [ "${#required_submodules[@]}" -eq 0 ]; then
  echo 'No build submodules found' >&2
  exit 1
fi
git submodule update --init --recursive -- "${required_submodules[@]}"
test -f src/lib/small/CMakeLists.txt
test -f third_party/luajit/CMakeLists.txt
