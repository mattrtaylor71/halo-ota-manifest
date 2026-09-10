#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Build separately with tools/build_ota_policy_production.py. This entry keeps
# the verified bytes unchanged through local packaging, staging and promotion.
exec "${PYTHON_BIN:-python3}" "$SCRIPT_DIR/tools/ota/publish_pair.py" "$@"
