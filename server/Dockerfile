# Homelab build. Context is the repo root: docker compose -f server/docker-compose.yml up -d --build
FROM node:22-alpine

RUN apk add --no-cache ffmpeg
WORKDIR /app
COPY server/package.json server/package-lock.json ./server/
RUN cd server && npm ci --omit=dev
COPY server/ ./server/
COPY site/ ./site/

ENV NODE_ENV=production PORT=8080 SITE_DIR=/app/site
USER node
EXPOSE 8080

HEALTHCHECK --interval=30s --timeout=3s --start-period=10s --retries=3 \
  CMD wget -q -O /dev/null http://127.0.0.1:8080/healthz || exit 1
CMD ["node", "server/server.js"]
