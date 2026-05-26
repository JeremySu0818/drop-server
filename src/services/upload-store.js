import crypto from 'node:crypto';

const DEFAULT_PURGE_BATCH_SIZE = 512;

export function createUploadStore({ ttlMs, now }) {
  const uploads = new Map();
  let purgeIterator = uploads.entries();

  function purgeExpired(batchSize = DEFAULT_PURGE_BATCH_SIZE) {
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

  function clearUploads() {
    const cleared = uploads.size;
    uploads.clear();
    purgeIterator = uploads.entries();
    return cleared;
  }

  function appendFiles(target, files) {
    for (let i = 0; i < files.length; i += 1) {
      target.push(files[i]);
    }
  }

  function upsertUpload(parsed) {
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
      files: parsed.files,
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
