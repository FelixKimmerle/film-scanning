#!/usr/bin/env bash
set -euo pipefail
BASE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
export SFL_HOME="$BASE/silverfast-data"
export SFL_CACHE="$BASE/silverfast-cache"
exec "$BASE/silverfast-bin/bin/silverfast-linux" "$@"
