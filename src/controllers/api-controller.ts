import type { Request, Response } from 'express';

import type { UploadStore } from '../services/upload-store.js';
import { readJsonBody } from '../utils/http-body.js';
import { readLookupKey, requireEncryptedPayload } from '../utils/validators.js';

type ApiControllerDependencies = {
  uploadStore: UploadStore;
  ttlMs: number;
};

async function readJsonOrRespondBadRequest(req: Request, res: Response) {
  try {
    return await readJsonBody(req);
  } catch (error) {
    if (error instanceof Error && error.message === 'INVALID_JSON') {
      res.status(400).json({ error: 'Invalid JSON body.' });
      return null;
    }
    throw error;
  }
}

export function createApiController({ uploadStore, ttlMs }: ApiControllerDependencies) {
  async function uploads(req: Request, res: Response) {
    uploadStore.purgeExpired();

    const body = await readJsonOrRespondBadRequest(req, res);
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

    const body = await readJsonOrRespondBadRequest(req, res);
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
      uploadStore.purgeExpired();
      res.json({
        ok: true,
        ttlMinutes: Math.round(ttlMs / 60000),
        stored: uploadStore.size(),
      });
    },
    reset(_req: Request, res: Response) {
      const cleared = uploadStore.clearUploads();
      res.json({ ok: true, cleared, stored: uploadStore.size() });
    },
    uploads,
    download,
  };
}
