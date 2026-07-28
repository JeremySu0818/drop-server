---
title: Drop Server
emoji: 📦
colorFrom: blue
colorTo: indigo
sdk: docker
app_port: 7860
---

# Drop Server

Drop is an encrypted, one-time download service. Express remains responsible for
HTTP, CORS, security headers, JSON validation, routing, and the React admin UI.
Encrypted upload retention is handled by a C++17 N-API core.

## Native security core

The native core owns an `std::unordered_map` containing decoded encrypted file
bytes, IVs, encrypted metadata, creation time, and expiry time. Its responsibilities
are:

- atomic upsert and take-once download behavior;
- TTL expiry enforcement;
- live session, file, and encrypted-byte statistics;
- immediate RAII destruction on download, expiry, reset, and addon finalization;
- `explicit_bzero` of every native encrypted byte buffer before it is released.

Node.js still briefly sees the incoming and outgoing base64 JSON because it owns
the HTTP protocol boundary, but it no longer retains encrypted uploads in V8
managed storage.

## Local development

The native build needs Python 3, `make`, and a C++17 compiler.

```bash
npm ci
npm run build
npm test
npm start
```

For watch mode:

```bash
npm run dev
```

If `native/drop_core.cc` changes while watch mode is running, restart watch mode
so the addon is rebuilt.

## Configuration

| Variable | Default | Purpose |
| --- | ---: | --- |
| `PORT` | `7860` | HTTP listen port |
| `TTL_MS` | `1800000` | Upload lifetime in milliseconds |
| `MAX_JSON_SIZE` | `25mb` | Maximum HTTP JSON body size |
| `ALLOWED_ORIGIN` | `*` | One origin or comma-separated CORS origins |
| `DROP_NATIVE_ADDON_PATH` | auto-detected | Optional absolute path to `drop_core.node` |

Files larger than a browser can safely hold in memory use the chunked API. Up to
three 64 MiB slices are encrypted and uploaded concurrently, then retained only
in server memory; plaintext filenames and file contents never reach the server.
Incomplete uploads expire after 30 minutes of inactivity, and the browser also
sends a best-effort abort request when its page is closed.

## Hugging Face Spaces

The repository uses the Docker Space SDK. The multi-stage Dockerfile compiles the
addon and TypeScript in a Debian Node 20 builder, prunes development packages,
then copies the addon into an ABI-compatible Node 20 runtime image. The final
container runs as the unprivileged `node` user and listens on port `7860`.
