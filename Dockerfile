FROM node:20-bookworm-slim AS build

WORKDIR /app

RUN apt-get update \
    && apt-get install -y --no-install-recommends g++ make python3 \
    && rm -rf /var/lib/apt/lists/*

COPY package*.json binding.gyp ./
COPY native ./native
RUN npm ci

COPY tsconfig.json server.ts ./
COPY src ./src
COPY public ./public

RUN npm run build \
    && npm prune --omit=dev

FROM node:20-bookworm-slim AS runtime

WORKDIR /app

ENV NODE_ENV=production
ENV PORT=7860

RUN chown node:node /app

COPY --from=build --chown=node:node /app/package*.json ./
COPY --from=build --chown=node:node /app/node_modules ./node_modules
COPY --from=build --chown=node:node /app/dist ./dist
COPY --from=build --chown=node:node /app/public ./public
COPY --from=build --chown=node:node /app/build/Release/drop_core.node ./build/Release/drop_core.node

USER node

EXPOSE 7860

CMD ["node", "dist/server.js"]
