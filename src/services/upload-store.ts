import crypto from 'node:crypto';

import type { EncryptedFilePayload, ValidatedEncryptedPayload } from '../types.js';

const DEFAULT_PURGE_BATCH_SIZE = 512;

type StoredFilePayload = {
  fileIv: Buffer;
  fileCiphertext: Buffer;
  metaIv: Buffer;
  metaCiphertext: Buffer;
};

type UploadRecord = {
  id: string;
  files: StoredFilePayload[];
  createdAt: number;
  expiresAt: number;
};

type UploadStoreMap = Map<string, UploadRecord>;

type UploadStoreDependencies = {
  ttlMs: number;
  now: () => number;
};

type UploadInsertPayload = {
  ok: true;
  expiresAt: number;
  files: number;
};

type UploadErrorPayload = {
  error: string;
};

export type UploadStore = {
  clearUploads: () => number;
  purgeExpired: (batchSize?: number) => void;
  size: () => number;
  takeDownload: (lookupKey: string) => { status: number; payload: UploadErrorPayload | { files: EncryptedFilePayload[] } };
  upsertUpload: (parsed: ValidatedEncryptedPayload) => { status: number; payload: UploadInsertPayload };
};

function compactFile(file: EncryptedFilePayload): StoredFilePayload {
  return {
    fileIv: Buffer.from(file.fileIv, 'base64'),
    fileCiphertext: Buffer.from(file.fileCiphertext, 'base64'),
    metaIv: Buffer.from(file.metaIv, 'base64'),
    metaCiphertext: Buffer.from(file.metaCiphertext, 'base64'),
  };
}

function expandFile(file: StoredFilePayload): EncryptedFilePayload {
  return {
    fileIv: file.fileIv.toString('base64'),
    fileCiphertext: file.fileCiphertext.toString('base64'),
    metaIv: file.metaIv.toString('base64'),
    metaCiphertext: file.metaCiphertext.toString('base64'),
  };
}

function compactFiles(files: EncryptedFilePayload[]): StoredFilePayload[] {
  const compacted = new Array<StoredFilePayload>(files.length);
  for (let i = 0; i < files.length; i += 1) {
    compacted[i] = compactFile(files[i]);
  }
  return compacted;
}

function expandFiles(files: StoredFilePayload[]): EncryptedFilePayload[] {
  const expanded = new Array<EncryptedFilePayload>(files.length);
  for (let i = 0; i < files.length; i += 1) {
    expanded[i] = expandFile(files[i]);
  }
  return expanded;
}

export function createUploadStore({ ttlMs, now }: UploadStoreDependencies): UploadStore {
  const uploads: UploadStoreMap = new Map();
  let purgeIterator = uploads.entries();

  function purgeExpired(batchSize = DEFAULT_PURGE_BATCH_SIZE): void {
    const current = now();
    let checked = 0;

    while (checked < batchSize) {
      const entry = purgeIterator.next();
      if (entry.done) {
        purgeIterator = uploads.entries();
        break;
      }

      const [lookupKey, record] = entry.value;
      checked += 1;
      if (record.expiresAt <= current) {
        uploads.delete(lookupKey);
      }
    }
  }

  function clearUploads(): number {
    const cleared = uploads.size;
    uploads.clear();
    purgeIterator = uploads.entries();
    return cleared;
  }

  function appendFiles(target: StoredFilePayload[], files: EncryptedFilePayload[]): void {
    for (let i = 0; i < files.length; i += 1) {
      target.push(compactFile(files[i]));
    }
  }

  function upsertUpload(parsed: ValidatedEncryptedPayload): {
    status: number;
    payload: UploadInsertPayload;
  } {
    const createdAt = now();
    const existing = uploads.get(parsed.key);

    if (existing) {
      if (existing.expiresAt <= createdAt) {
        uploads.delete(parsed.key);
      } else {
        appendFiles(existing.files, parsed.files);
        return {
          status: 200,
          payload: {
            ok: true,
            expiresAt: existing.expiresAt,
            files: existing.files.length,
          },
        };
      }
    }

    const expiresAt = createdAt + ttlMs;
    uploads.set(parsed.key, {
      id: crypto.randomUUID(),
      files: compactFiles(parsed.files),
      createdAt,
      expiresAt,
    });

    return {
      status: 201,
      payload: { ok: true, expiresAt, files: parsed.files.length },
    };
  }

  function takeDownload(
    lookupKey: string,
  ): { status: number; payload: UploadErrorPayload | { files: EncryptedFilePayload[] } } {
    const record = uploads.get(lookupKey);
    if (!record) {
      return {
        status: 404,
        payload: {
          error: 'Image not found. It may have already been downloaded or expired.',
        },
      };
    }

    uploads.delete(lookupKey);

    if (record.expiresAt <= now()) {
      return { status: 410, payload: { error: 'Image has expired.' } };
    }

    return { status: 200, payload: { files: expandFiles(record.files) } };
  }

  function size(): number {
    return uploads.size;
  }

  return {
    clearUploads,
    purgeExpired,
    size,
    takeDownload,
    upsertUpload,
  };
}
