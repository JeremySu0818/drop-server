const DEFAULT_PORT = 7860;
const DEFAULT_TTL_MS = 30 * 60 * 1000;
const PURGE_INTERVAL_CAP_MS = 60 * 1000;

export const runtimeConfig = Object.freeze({
  port: Number(process.env.PORT || DEFAULT_PORT),
  ttlMs: Number(process.env.TTL_MS || DEFAULT_TTL_MS),
  allowedOrigin: process.env.ALLOWED_ORIGIN || '*',
});

export function getPurgeInterval(ttlMs) {
  return Math.min(ttlMs, PURGE_INTERVAL_CAP_MS);
}
