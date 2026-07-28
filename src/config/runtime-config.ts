const DEFAULT_PORT = 7860;
const DEFAULT_TTL_MS = 30 * 60 * 1000;
const DEFAULT_MAX_JSON_BYTES = 25 * 1024 * 1024;
const PURGE_INTERVAL_CAP_MS = 60 * 1000;

export type RuntimeConfig = Readonly<{
  port: number;
  ttlMs: number;
  maxJsonBytes: number;
  allowedOrigin: string;
}>;

function readPositiveInteger(
  name: string,
  rawValue: string | undefined,
  fallback: number,
): number {
  if (rawValue === undefined || rawValue.trim() === '') {
    return fallback;
  }
  const value = Number(rawValue);
  if (!Number.isSafeInteger(value) || value <= 0) {
    throw new Error(`${name} must be a positive integer.`);
  }
  return value;
}

function readByteSize(
  name: string,
  rawValue: string | undefined,
  fallback: number,
): number {
  if (rawValue === undefined || rawValue.trim() === '') {
    return fallback;
  }
  const match = rawValue.trim().match(/^(\d+(?:\.\d+)?)\s*(b|kb|mb|gb)?$/i);
  if (!match) {
    throw new Error(`${name} must be a byte count such as 26214400 or 25mb.`);
  }
  const units: Record<string, number> = {
    b: 1,
    kb: 1024,
    mb: 1024 ** 2,
    gb: 1024 ** 3,
  };
  const value = Number(match[1]) * units[(match[2] || 'b').toLowerCase()];
  if (!Number.isSafeInteger(value) || value <= 0) {
    throw new Error(`${name} must resolve to a positive safe integer.`);
  }
  return value;
}

const port = readPositiveInteger('PORT', process.env.PORT, DEFAULT_PORT);
if (port > 65535) {
  throw new Error('PORT must be between 1 and 65535.');
}

export const runtimeConfig: RuntimeConfig = Object.freeze({
  port,
  ttlMs: readPositiveInteger('TTL_MS', process.env.TTL_MS, DEFAULT_TTL_MS),
  maxJsonBytes: readByteSize(
    'MAX_JSON_SIZE',
    process.env.MAX_JSON_SIZE,
    DEFAULT_MAX_JSON_BYTES,
  ),
  allowedOrigin: process.env.ALLOWED_ORIGIN || '*',
});

export function getPurgeInterval(ttlMs: number): number {
  return Math.min(ttlMs, PURGE_INTERVAL_CAP_MS);
}
