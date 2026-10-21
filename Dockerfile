FROM nginx:alpine

COPY nginx.conf /etc/nginx/nginx.conf
COPY spirograph.html /usr/share/nginx/html/index.html
# JetBrains Mono (SIL OFL 1.1), same file Dynacat serves
COPY fonts/JetBrainsMono-Regular.woff2 /usr/share/nginx/html/

USER nginx
EXPOSE 8080

HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
  CMD wget -q -O /dev/null http://127.0.0.1:8080/healthz || exit 1
