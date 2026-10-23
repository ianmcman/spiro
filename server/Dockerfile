# Homelab build. Context is the repo root: docker compose -f server/docker-compose.yml up -d --build
# Debian, not Alpine: the musl canvas binary needs AVX and crashes (SIGILL) on the Westmere Xeon in arrstack.
FROM node:22-slim

RUN apt-get update && apt-get install -y --no-install-recommends ffmpeg && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY server/package.json server/package-lock.json ./server/
RUN cd server && npm ci --omit=dev
COPY server/ ./server/
COPY site/ ./site/
RUN mkdir -p server/models
# U2-Net (small) for "Cut out subject"; Apache-2.0
ADD --chmod=644 --checksum=sha256:309c8469258dda742793dce0ebea8e6dd393174f89934733ecc8b14c76f4ddd8 https://github.com/danielgatis/rembg/releases/download/v0.0.0/u2netp.onnx /app/server/models/u2netp.onnx

ENV NODE_ENV=production PORT=8080 SITE_DIR=/app/site DATA_DIR=/data
USER node
EXPOSE 8080

HEALTHCHECK --interval=30s --timeout=3s --start-period=10s --retries=3 \
  CMD node -e "fetch('http://127.0.0.1:8080/healthz').then(r=>process.exit(r.ok?0:1),()=>process.exit(1))"
CMD ["node", "server/server.js"]
