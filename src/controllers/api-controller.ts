import type { Request, Response } from 'express';

import type { UploadStore } from '../services/upload-store.js';
import type { ChunkedUploadStore } from '../services/chunked-upload-store.js';
import {
  INVALID_JSON,
  PAYLOAD_TOO_LARGE,
  readJsonBody,
} from '../utils/http-body.js';
import { readLookupKey, requireEncryptedPayload } from '../utils/validators.js';

type ApiControllerDependencies = {
  uploadStore: UploadStore;
  chunkedUploadStore: ChunkedUploadStore;
  ttlMs: number;
  maxJsonBytes: number;
};

async function readJsonOrRespondBadRequest(
  req: Request,
  res: Response,
  maxJsonBytes: number,
) {
  try {
    return await readJsonBody(req, maxJsonBytes);
  } catch (error) {
    if (error instanceof Error && error.message === INVALID_JSON) {
      res.status(400).json({ error: 'Invalid JSON body.' });
      return null;
    }
    if (error instanceof Error && error.message === PAYLOAD_TOO_LARGE) {
      res.status(413).json({ error: 'JSON body exceeds the configured limit.' });
      return null;
    }
    throw error;
  }
}

export function createApiController({
  uploadStore,
  chunkedUploadStore,
  ttlMs,
  maxJsonBytes,
}: ApiControllerDependencies) {
  async function uploads(req: Request, res: Response) {
    uploadStore.purgeExpired();

    const body = await readJsonOrRespondBadRequest(req, res, maxJsonBytes);
    if (!body) {
      return;
    }

    const parsed = requireEncryptedPayload(body);
    if ('error' in parsed) {
      res.status(400).json({ error: parsed.error });
      return;
    }

    const result = uploadStore.upsertUpload(parsed);
    res.status(result.status).json(result.payload);
  }

  async function download(req: Request, res: Response) {
    uploadStore.purgeExpired();

    const body = await readJsonOrRespondBadRequest(req, res, maxJsonBytes);
    if (!body) {
      return;
    }

    const lookup = readLookupKey(body);
    if ('error' in lookup) {
      res.status(400).json({ error: 'lookupKey must be a SHA-256 hex digest.' });
      return;
    }

    const result = uploadStore.takeDownload(lookup.key);
    res.status(result.status).json(result.payload);
  }

  return {
    health(_req: Request, res: Response) {
      const stats = uploadStore.getStats();
      res.json({
        ok: true,
        ttlMinutes: Math.round(ttlMs / 60000),
        stored: stats.uploadCount + chunkedUploadStore.getStats().uploadCount,
      });
    },
    reset(_req: Request, res: Response) {
      const cleared = uploadStore.clearUploads();
      const chunkedCleared = chunkedUploadStore.clearUploads();
      res.json({ ok: true, cleared: cleared + chunkedCleared, stored: 0 });
    },
    uploads,
    download,
  };
}
