#!/bin/sh
# Local helper for the spirograph container. Run it on the Docker host.
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
