#!/usr/bin/env bash
# Start the deterministic mock API, run `make test`, stop the mock again.
#
#   ./test/run.sh    # runs every test/sql/*.test against the mock
set -euo pipefail

PROJ_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${JEV_MOCK_PORT:-8765}"

# The "no API key" case asserts that nothing is configured anywhere.
unset TYPESAFE_API_KEY || true

python3 "${PROJ_DIR}/test/mock_api.py" "${PORT}" &
MOCK_PID=$!
trap 'kill "${MOCK_PID}" 2>/dev/null || true' EXIT

for _ in $(seq 1 50); do
	if curl -fsS "http://127.0.0.1:${PORT}/" >/dev/null 2>&1; then
		break
	fi
	sleep 0.1
done

make -C "${PROJ_DIR}" test
