#!/usr/bin/env bash
set -euo pipefail
BASE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$BASE"
if [ ! -x ./nobcpp ]; then
    c++ -std=c++20 -o nobcpp nobcpp.cpp
fi
./nobcpp release
export PYTHONPATH="$BASE"
exec python3 -m app.server --host "${HOST:-0.0.0.0}" --port "${PORT:-8765}" "$@"
