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
#
# One divergence is deliberate and is allowed here by name: the ``messages``
# count. This implementation exempts the side that is flattening out of an
# ``extreme`` regime from ``max_behind_ticks``, so that side is quoted and
# cancelled instead of being dropped. Without the exemption the 4.0 spread
# multiplier at ``extreme`` puts the quote past the cap and ``reduce_only``
# never places anything -- a bug the Python reference still has. It costs
# messages and no fills on this tape. Every other line must still match,
# and any other difference fails.
#
# The C++ side is run with ``--control both`` because bare ``--control``
# here runs the gate, skew and both arms; ``both`` is the single arm the
# Python flag means.

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
echo "c++:    $(cd "${here}" && "${binary}" replay -c "${config}" --no-journal --control both 2>/dev/null | tee "${tmp}/cpp.txt" | grep -c fills) summary lines"

if diff -u "${tmp}/python.txt" "${tmp}/cpp.txt" > "${tmp}/diff.txt"; then
    echo "PARITY: the two implementations agree on every line"
    exit 0
fi

cat "${tmp}/diff.txt"

# Changed content lines, without the +++/--- file headers.
changed="$(grep -E '^[+-]' "${tmp}/diff.txt" | grep -Ev '^(\+\+\+|---)' || true)"
unexpected="$(printf '%s\n' "${changed}" | grep -Ev '^[+-]messages: ' || true)"

if [[ -z "${unexpected//[[:space:]]/}" ]]; then
    echo "PARITY: agree on every line but the message count, which is the"
    echo "        extreme reduce-only fix described at the top of this script."
    exit 0
fi

echo "MISMATCH: see the diff above" >&2
exit 1
