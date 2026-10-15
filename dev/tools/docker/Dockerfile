# Clausewitz LLM sidecar (dev/tools) as a container. Build context = dev/tools:
#   docker build -f docker/Dockerfile -t clausewitz-sidecar .
# Ignore rules: docker/Dockerfile.dockerignore (BuildKit picks it up next to this Dockerfile).
FROM node:22-alpine

WORKDIR /app
COPY package.json package-lock.json ./
RUN npm ci --omit=dev && npm cache clean --force
COPY . .

# Settings + runtime data live on the /data volume, not in the image:
#   /data/sidecar-config.json  settings (API key, game token, admin password hash) written by the dashboard
#   /data/cache/               map + world caches, sessions, loadouts store
# CLW_HOST=0.0.0.0: listen on all interfaces of the container (publish the port only where it is needed).
ENV NODE_ENV=production \
    CLW_HOST=0.0.0.0 \
    CLW_PORT=8788 \
    CLW_CONFIG=/data/sidecar-config.json \
    CLW_DATA_DIR=/data/cache
RUN mkdir -p /data/cache && chown -R node:node /data
VOLUME ["/data"]
USER node
EXPOSE 8788

# "/" serves the dashboard page and is never token-gated (unlike /health, a game route when a game token is set)
HEALTHCHECK --interval=30s --timeout=5s --start-period=20s --retries=3 \
  CMD node -e "fetch('http://127.0.0.1:'+(process.env.CLW_PORT||8788)+'/').then(r=>process.exit(r.status<500?0:1)).catch(()=>process.exit(1))"

CMD ["node", "llm_sidecar.mjs"]
