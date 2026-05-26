import { readJsonBody } from '../utils/http-body.js';
import { readLookupKey, requireEncryptedPayload } from '../utils/validators.js';

async function readJsonOrRespondBadRequest(req, res) {
  try {
    return await readJsonBody(req);
  } catch (error) {
    if (error?.message === 'INVALID_JSON') {
      res.status(400).json({ error: 'Invalid JSON body.' });
      return null;
    }
    throw error;
  }
}

export function createApiController({ uploadStore, ttlMs }) {
  function health(_req, res) {
    uploadStore.purgeExpired();
    res.json({
      ok: true,
      ttlMinutes: Math.round(ttlMs / 60000),
      stored: uploadStore.size(),
    });
  }

  function reset(_req, res) {
    const cleared = uploadStore.clearUploads();
    res.json({ ok: true, cleared, stored: uploadStore.size() });
  }

  async function uploads(req, res) {
    uploadStore.purgeExpired();

    const body = await readJsonOrRespondBadRequest(req, res);
    if (!body) {
      return;
    }

    const parsed = requireEncryptedPayload(body);
    if (parsed.error) {
      res.status(400).json({ error: parsed.error });
      return;
    }

    const result = uploadStore.upsertUpload(parsed);
    res.status(result.status).json(result.payload);
  }

  async function download(req, res) {
    uploadStore.purgeExpired();

    const body = await readJsonOrRespondBadRequest(req, res);
    if (!body) {
      return;
    }

    const lookup = readLookupKey(body || {});
    if (lookup.error) {
      res.status(400).json({ error: 'lookupKey must be a SHA-256 hex digest.' });
      return;
    }

    const result = uploadStore.takeDownload(lookup.key);
    res.status(result.status).json(result.payload);
  }

  return {
    download,
    health,
    reset,
    uploads,
  };
}
