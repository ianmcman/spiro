#!/bin/sh
# Helper for the homelab container. Run it on the Docker host, from a checkout that has both site/ and server/.
set -eu
cd "$(dirname "$0")"

case "${1:-}" in
  build)    docker compose build ;;
  up)       docker compose up -d ;;
  down)     docker compose down ;;
  logs)     docker compose logs -f --tail=100 ;;
  redeploy) docker compose up -d --build ;;
  *)
    echo "usage: $0 {build|up|down|logs|redeploy}" >&2
    exit 1
    ;;
esac
