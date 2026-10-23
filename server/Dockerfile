# Homelab build. Context is the repo root: docker compose -f server/docker-compose.yml up -d --build
# Debian, not Alpine: the musl canvas binary needs AVX and crashes (SIGILL) on the Westmere Xeon in arrstack.
FROM node:22-slim

RUN apt-get update && apt-get install -y --no-install-recommends ffmpeg && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY server/package.json server/package-lock.json ./server/
RUN cd server && npm ci --omit=dev
COPY server/ ./server/
COPY site/ ./site/

ENV NODE_ENV=production PORT=8080 SITE_DIR=/app/site
USER node
EXPOSE 8080

HEALTHCHECK --interval=30s --timeout=3s --start-period=10s --retries=3 \
  CMD node -e "fetch('http://127.0.0.1:8080/healthz').then(r=>process.exit(r.ok?0:1),()=>process.exit(1))"
CMD ["node", "server/server.js"]
