#!/usr/bin/env bash
#
# One-shot VPS setup for the C++ spread harvester's forward walk. Idempotent:
# safe to re-run.
#
#   sudo ./deploy/bootstrap.sh
#
# Creates a service user, clones the repo, downloads IBKR's TWS API C++
# client, builds the binary with the Databento feed and the IBKR router,
# installs the systemd units. It does NOT start anything and it does NOT
# install IB Gateway -- run deploy/install-ibc.sh for that, then
# `harvester doctor` before you let it trade.

set -euo pipefail

REPO_URL="${REPO_URL:-https://github.com/vivere7108-lab/CME-Market-Maker-C.git}"
BRANCH="${BRANCH:-main}"
SERVICE_USER="${SERVICE_USER:-harvester}"
INSTALL_DIR="${INSTALL_DIR:-/opt/harvester}"
# IBKR's stable-channel API. 10.30 needs no protobuf; 10.37+ does, and the
# build handles either as long as protobuf-compiler is installed.
TWS_API_VERSION="${TWS_API_VERSION:-1030.01}"
TWS_API_URL="${TWS_API_URL:-https://interactivebrokers.github.io/downloads/twsapi_macunix.${TWS_API_VERSION}.zip}"

log() { printf '\n== %s\n' "$*"; }

if [[ $EUID -ne 0 ]]; then
    echo "run as root: sudo $0" >&2
    exit 1
fi

log "system packages"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
# xvfb/openbox are for IB Gateway: it is a Java GUI app with no headless mode,
# so it needs a virtual display even when nobody is looking at it.
apt-get install -yqq \
    build-essential cmake ninja-build git curl unzip pkg-config \
    libyaml-cpp-dev libssl-dev libzstd-dev nlohmann-json3-dev \
    libprotobuf-dev protobuf-compiler \
    xvfb x11vnc openbox \
    tzdata

log "service user: ${SERVICE_USER}"
if ! id -u "${SERVICE_USER}" >/dev/null 2>&1; then
    useradd --system --create-home --home-dir "/home/${SERVICE_USER}" \
            --shell /usr/sbin/nologin "${SERVICE_USER}"
else
    echo "already exists"
fi

log "repository: ${INSTALL_DIR} (${BRANCH})"

# Git refuses to operate on a repository owned by someone else ("detected
# dubious ownership"), which is exactly what this script creates: it clones,
# then chowns to the service user. Run git AS the owner, with safe.directory
# passed inline (not into anyone's global config).
as_owner() {
    sudo -u "${SERVICE_USER}" env HOME="/home/${SERVICE_USER}" \
        git -c safe.directory="${INSTALL_DIR}" "$@"
}

mkdir -p "${INSTALL_DIR}"
chown -R "${SERVICE_USER}:${SERVICE_USER}" "${INSTALL_DIR}"

if [[ -d "${INSTALL_DIR}/.git" ]]; then
    as_owner -C "${INSTALL_DIR}" fetch --quiet origin "${BRANCH}"
    as_owner -C "${INSTALL_DIR}" checkout --quiet "${BRANCH}"
    as_owner -C "${INSTALL_DIR}" reset --hard --quiet "origin/${BRANCH}"
else
    as_owner clone --quiet --branch "${BRANCH}" "${REPO_URL}" "${INSTALL_DIR}"
fi
chown -R "${SERVICE_USER}:${SERVICE_USER}" "${INSTALL_DIR}"
echo "  now at $(as_owner -C "${INSTALL_DIR}" log -1 --format='%h %s')"

log "IBKR TWS API ${TWS_API_VERSION}"
# The API is distributed by IBKR under its own licence
# (https://interactivebrokers.github.io/) and is not part of this
# repository; it is downloaded here and never committed.
TWS_DIR="${INSTALL_DIR}/third_party/twsapi"
if [[ -f "${TWS_DIR}/IBJts/source/cppclient/client/EClientSocket.h" ]]; then
    echo "already present: $(cat "${TWS_DIR}/IBJts/API_VersionNum.txt" 2>/dev/null || echo '?')"
else
    tmp="$(mktemp -d)"
    curl -fsSL "${TWS_API_URL}" -o "${tmp}/twsapi.zip"
    mkdir -p "${TWS_DIR}"
    unzip -oq "${tmp}/twsapi.zip" -d "${TWS_DIR}"
    rm -rf "${tmp}"
    chown -R "${SERVICE_USER}:${SERVICE_USER}" "${TWS_DIR}"
    echo "  installed $(cat "${TWS_DIR}/IBJts/API_VersionNum.txt")"
fi

log "build"
# databento-cpp is fetched by CMake if the box has no installed copy; the
# first build therefore needs network access and takes a few minutes.
sudo -u "${SERVICE_USER}" cmake -S "${INSTALL_DIR}" -B "${INSTALL_DIR}/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DHARVESTER_WITH_DATABENTO=ON \
    -DHARVESTER_WITH_IBKR=ON \
    -DTWS_API_DIR="${TWS_DIR}/IBJts/source/cppclient/client" \
    -DHARVESTER_BUILD_TESTS=ON \
    -DHARVESTER_BUILD_BENCH=OFF
sudo -u "${SERVICE_USER}" cmake --build "${INSTALL_DIR}/build" -j "$(nproc)"
sudo -u "${SERVICE_USER}" "${INSTALL_DIR}/build/harvester_tests" > /dev/null && echo "  tests pass"

log "run directory"
install -d -o "${SERVICE_USER}" -g "${SERVICE_USER}" -m 750 "${INSTALL_DIR}/runs"

log "systemd units"
for unit in ibc.service harvester.service; do
    sed -e "s|@INSTALL_DIR@|${INSTALL_DIR}|g" \
        -e "s|@SERVICE_USER@|${SERVICE_USER}|g" \
        "${INSTALL_DIR}/deploy/${unit}" > "/etc/systemd/system/${unit}"
done
if [[ ! -f /etc/harvester.env ]]; then
    install -o root -g "${SERVICE_USER}" -m 640 \
        "${INSTALL_DIR}/deploy/harvester.env.example" /etc/harvester.env
    echo "wrote /etc/harvester.env -- review it before starting"
else
    echo "/etc/harvester.env exists; left alone"
fi
systemctl daemon-reload

cat <<EOT

Bootstrap done. Nothing is running yet, by design.

Next:
  1. sudo ${INSTALL_DIR}/deploy/install-ibc.sh      # IB Gateway + IBC
  2. sudo nano /etc/ibc/config.ini                  # paper credentials, mode=paper
  3. sudo systemctl enable --now ibc                # start the gateway
  4. sudo -u ${SERVICE_USER} env \$(grep DATABENTO /etc/harvester.env) \\
         ${INSTALL_DIR}/build/harvester doctor \\
         -c ${INSTALL_DIR}/configs/es_paper.yaml    # must pass before quoting
  5. sudo systemctl enable --now harvester        # starts in --dry-run

Read ${INSTALL_DIR}/deploy/README.md before step 5.
EOT
