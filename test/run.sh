#!/usr/bin/env bash
# Start the deterministic mock API, run `make test`, stop the mock again.
#
#   ./test/run.sh    # runs every test/sql/*.test against the mock
set -euo pipefail

PROJ_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# One source of truth for the port: test/mock_api.py.
DEFAULT_PORT="$(python3 -c "import sys; sys.path.insert(0, '${PROJ_DIR}/test'); import mock_api; print(mock_api.DEFAULT_PORT)")"
PORT="${JEV_MOCK_PORT:-${DEFAULT_PORT}}"

if ! grep -q "127.0.0.1:${PORT}/v1/systemone" "${PROJ_DIR}/test/sql/jev.test"; then
	echo "test/run.sh: test/sql/jev.test does not point at 127.0.0.1:${PORT}" >&2
	exit 1
fi

# The tests are skipped unless this says the mock is up (test/sql/jev.test requires it),
# and the "no API key" case asserts that nothing is configured anywhere.
export JEV_MOCK_RUNNING=1
unset TYPESAFE_API_KEY || true

python3 "${PROJ_DIR}/test/mock_api.py" "${PORT}" &
MOCK_PID=$!
trap 'kill "${MOCK_PID}" 2>/dev/null || true' EXIT

# Probe the real endpoint with a real request - a listener that is not our mock, or a mock
# that cannot answer, has to fail here rather than half way through the suite.
probe_body='{"model":"jev-latest","state":{"condition":"probe ok","rows":[{"probe":"ok"}]},"questions":{"r0":{"type":"noul","instructions":"probe"}}}'
for attempt in $(seq 1 50); do
	if curl -fsS -X POST "http://127.0.0.1:${PORT}/v1/systemone" \
		-H 'Authorization: Bearer test-key' \
		-H 'Content-Type: application/json' \
		-d "${probe_body}" 2>/dev/null | grep -q '"answers"'; then
		break
	fi
	if [ "${attempt}" -eq 50 ]; then
		echo "test/run.sh: no mock API answering on http://127.0.0.1:${PORT}/v1/systemone" >&2
		exit 1
	fi
	sleep 0.1
done

make -C "${PROJ_DIR}" test
