import { createRequire } from 'node:module';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

import type { EncryptedFilePayload, ValidatedEncryptedPayload } from '../types.js';

type UploadInsertPayload = {
  ok: true;
  expiresAt: number;
  files: number;
};

type UploadErrorPayload = {
  error: string;
};

export type UploadStoreStats = {
  uploadCount: number;
  fileCount: number;
  encryptedBytes: number;
  capacityBytes: number;
};

type NativeResult<T> = {
  status: number;
  payload: T;
};

type NativeUploadStore = {
  clearUploads: () => number;
  getStats: () => UploadStoreStats;
  purgeExpired: () => number;
  takeDownload: (
    lookupKey: string,
  ) => NativeResult<UploadErrorPayload | { files: EncryptedFilePayload[] }>;
  upsertUpload: (
    lookupKey: string,
    files: EncryptedFilePayload[],
  ) => NativeResult<UploadErrorPayload | UploadInsertPayload>;
};

type NativeAddon = {
  createUploadStore: (ttlMs: number, capacityBytes: number) => NativeUploadStore;
};

type UploadStoreDependencies = {
  ttlMs: number;
  maxStoreBytes: number;
};

export type UploadStore = {
  clearUploads: () => number;
  getStats: () => UploadStoreStats;
  purgeExpired: () => void;
  takeDownload: (
    lookupKey: string,
  ) => NativeResult<UploadErrorPayload | { files: EncryptedFilePayload[] }>;
  upsertUpload: (
    parsed: ValidatedEncryptedPayload,
  ) => NativeResult<UploadErrorPayload | UploadInsertPayload>;
};

let cachedAddon: NativeAddon | undefined;

function loadNativeAddon(): NativeAddon {
  if (cachedAddon) {
    return cachedAddon;
  }

  const moduleDir = path.dirname(fileURLToPath(import.meta.url));
  const candidates = [
    process.env.DROP_NATIVE_ADDON_PATH,
    path.resolve(process.cwd(), 'build/Release/drop_core.node'),
    path.resolve(moduleDir, '../../build/Release/drop_core.node'),
    path.resolve(moduleDir, '../../../build/Release/drop_core.node'),
  ].filter((candidate): candidate is string => Boolean(candidate));
  const require = createRequire(import.meta.url);
  const failures: string[] = [];

  for (const candidate of [...new Set(candidates)]) {
    try {
      cachedAddon = require(candidate) as NativeAddon;
      return cachedAddon;
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error);
      failures.push(`${candidate}: ${message}`);
    }
  }

  throw new Error(
    [
      'Unable to load the Drop native security core.',
      'Run "npm run build:native" before starting the server.',
      ...failures,
    ].join('\n'),
  );
}

export function createUploadStore({
  ttlMs,
  maxStoreBytes,
}: UploadStoreDependencies): UploadStore {
  const nativeStore = loadNativeAddon().createUploadStore(ttlMs, maxStoreBytes);

  return {
    clearUploads: () => nativeStore.clearUploads(),
    getStats: () => nativeStore.getStats(),
    purgeExpired: () => {
      nativeStore.purgeExpired();
    },
    takeDownload: (lookupKey) => nativeStore.takeDownload(lookupKey),
    upsertUpload: (parsed) =>
      nativeStore.upsertUpload(parsed.key, parsed.files),
  };
}
