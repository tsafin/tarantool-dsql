#!/usr/bin/env bash
# Initialize build dependencies without the unpublished test-run gitlink.
# SQL and SQL-luatest adapters use test/test-run.py, so provision a known
# published runner revision separately from its repository gitlink.
set -euo pipefail

TEST_RUN_SHA=71c1373f9b64088cee1e8aba4d446bd42dca925c
TEST_RUN_URL=https://github.com/tarantool/test-run.git

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

if [ -e test-run/.git ]; then
  if [ "$(git -C test-run rev-parse HEAD)" != "$TEST_RUN_SHA" ]; then
    echo 'Existing test-run checkout does not match the parity runner pin' >&2
    exit 1
  fi
else
  if [ -d test-run ] && [ -n "$(ls -A test-run)" ]; then
    echo 'Cannot provision test-run into a non-empty directory' >&2
    exit 1
  fi
  git clone --quiet "$TEST_RUN_URL" test-run
  git -C test-run checkout --quiet --detach "$TEST_RUN_SHA"
fi
git -C test-run submodule update --init --recursive
test -f test-run/lib/luatest/luatest/cli_entrypoint.lua
test -f test-run/lib/msgpack-python/msgpack/__init__.py
test -f test-run/lib/tarantool-python/tarantool/__init__.py
