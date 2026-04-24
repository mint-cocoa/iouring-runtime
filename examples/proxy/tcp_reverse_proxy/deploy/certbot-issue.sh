#!/usr/bin/env bash
set -euo pipefail

ENV_FILE="${1:-/etc/iouring-runtime/tcp_reverse_proxy.env}"
if [[ ! -f "${ENV_FILE}" ]]; then
    echo "missing env file: ${ENV_FILE}" >&2
    exit 1
fi

# shellcheck disable=SC1090
source "${ENV_FILE}"

: "${TCP_PROXY_CERTBOT_DOMAIN:?set TCP_PROXY_CERTBOT_DOMAIN in the env file}"
: "${TCP_PROXY_CERTBOT_EMAIL:?set TCP_PROXY_CERTBOT_EMAIL in the env file}"
: "${TCP_PROXY_CERTBOT_CHALLENGE_WEBROOT:?set TCP_PROXY_CERTBOT_CHALLENGE_WEBROOT in the env file}"

CERTBOT_BIN="${CERTBOT_BIN:-certbot}"
DOMAIN_ARGS=(-d "${TCP_PROXY_CERTBOT_DOMAIN}")

if [[ -n "${TCP_PROXY_CERTBOT_EXTRA_DOMAINS:-}" ]]; then
    # Split on spaces so the env file can define: "www.example.com api.example.com"
    read -r -a extra_domains <<< "${TCP_PROXY_CERTBOT_EXTRA_DOMAINS}"
    for domain in "${extra_domains[@]}"; do
        [[ -n "${domain}" ]] || continue
        DOMAIN_ARGS+=(-d "${domain}")
    done
fi

install -d -m 0755 "${TCP_PROXY_CERTBOT_CHALLENGE_WEBROOT}"

exec "${CERTBOT_BIN}" certonly \
    --webroot \
    --webroot-path "${TCP_PROXY_CERTBOT_CHALLENGE_WEBROOT}" \
    --email "${TCP_PROXY_CERTBOT_EMAIL}" \
    --agree-tos \
    --non-interactive \
    --keep-until-expiring \
    "${DOMAIN_ARGS[@]}"
