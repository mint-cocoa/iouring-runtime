#!/usr/bin/env bash
set -euo pipefail

ENV_FILE="${1:-/etc/iouring-runtime/tcp_reverse_proxy.env}"
if [[ ! -f "${ENV_FILE}" ]]; then
    echo "missing env file: ${ENV_FILE}" >&2
    exit 1
fi

# shellcheck disable=SC1090
source "${ENV_FILE}"

SERVICE_NAME="${TCP_PROXY_SYSTEMD_SERVICE:-tcp_reverse_proxy.service}"

if ! command -v systemctl >/dev/null 2>&1; then
    echo "systemctl not found; cannot reload ${SERVICE_NAME}" >&2
    exit 1
fi

exec systemctl reload "${SERVICE_NAME}"
