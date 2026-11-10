#!/usr/bin/env bash
set -u
cd "$(dirname "$0")/.."
for bin in tanks_unnamed tanks_named tanks_independent tanks_queue tanks_worker; do
    if [[ ! -x $bin ]]; then echo "FAIL: нет исполняемого файла $bin"; exit 1; fi
done
exec python3 -I tests/run_tests.py
