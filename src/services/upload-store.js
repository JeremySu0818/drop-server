import crypto from 'node:crypto';

export function createUploadStore({ ttlMs, now }) {
  const uploads = new Map();

  function purgeExpired() {
    const current = now();
    for (const [lookupKey, record] of uploads.entries()) {
      if (record.expiresAt <= current) {
        uploads.delete(lookupKey);
      }
    }
  }

  function clearUploads() {
    const cleared = uploads.size;
    uploads.clear();
    return cleared;
  }

  function upsertUpload(parsed) {
    const createdAt = now();
    const existing = uploads.get(parsed.key);

    if (existing) {
      if (existing.expiresAt <= createdAt) {
        uploads.delete(parsed.key);
      } else {
        existing.files.push(...parsed.files);
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
      files: [...parsed.files],
      createdAt,
      expiresAt,
    });

    return {
      status: 201,
      payload: { ok: true, expiresAt, files: parsed.files.length },
    };
  }

  function takeDownload(lookupKey) {
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

    return { status: 200, payload: { files: record.files } };
  }

  function size() {
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
