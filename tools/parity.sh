#!/usr/bin/env bash
#
# Run the Python reference and this implementation on the same generated
# tape and diff their summaries. Identical output is the translation's
# correctness check; a difference is a bug in one of them.
#
#   tools/parity.sh [path-to-CME-Futures-Spread-Harvester] [python-with-harvester]
#
# The Python side needs the reference repository installed (pip install -e
# '.[dev]' in a venv); the C++ side needs a built ``harvester`` in build/.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
pyrepo="${1:-${here}/../CME-Futures-Spread-Harvester}"
python="${2:-${pyrepo}/.venv/bin/python}"
binary="${HARVESTER_BIN:-${here}/build/harvester}"
config="configs/es_replay.yaml"

if [[ ! -x "${binary}" ]]; then
    echo "no harvester binary at ${binary}; build first (cmake -B build && cmake --build build)" >&2
    exit 1
fi
if [[ ! -d "${pyrepo}" ]]; then
    echo "no Python reference at ${pyrepo}" >&2
    exit 1
fi

tmp="$(mktemp -d)"
trap 'rm -rf "${tmp}"' EXIT

echo "python: $(cd "${pyrepo}" && "${python}" -m harvester.cli replay -c "${config}" --no-journal --control 2>/dev/null | tee "${tmp}/python.txt" | grep -c fills) summary lines"
echo "c++:    $(cd "${here}" && "${binary}" replay -c "${config}" --no-journal --control 2>/dev/null | tee "${tmp}/cpp.txt" | grep -c fills) summary lines"

if diff -u "${tmp}/python.txt" "${tmp}/cpp.txt"; then
    echo "PARITY: the two implementations agree on every line"
else
    echo "MISMATCH: see the diff above" >&2
    exit 1
fi
