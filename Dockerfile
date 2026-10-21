FROM nginx:alpine

COPY nginx.conf /etc/nginx/nginx.conf
# site/ is also what Cloudflare Pages publishes (_headers is Pages-only; nginx.conf sets the same headers)
COPY site/ /usr/share/nginx/html/

USER nginx
EXPOSE 8080

HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
  CMD wget -q -O /dev/null http://127.0.0.1:8080/healthz || exit 1
