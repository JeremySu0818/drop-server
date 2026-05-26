import type {
  EncryptedFilePayload,
  LookupResult,
  ValidatedEncryptedPayload,
  ValidationError,
} from '../types.js';

export function isLookupKey(value: unknown): value is string {
  return typeof value === 'string' && /^[a-f0-9]{64}$/i.test(value);
}

export function isBase64(value: unknown): value is string {
  return (
    typeof value === 'string' &&
    value.length > 0 &&
    /^[A-Za-z0-9+/]+={0,2}$/.test(value)
  );
}

export function readLookupKey(body: Record<string, unknown>): LookupResult | ValidationError {
  const value = body.lookupKey ?? body.lookupHash ?? '';
  const key = String(value).toLowerCase();
  if (!isLookupKey(key)) {
    return { error: 'lookupKey must be a SHA-256 hex digest.' };
  }
  return { key };
}

function isEncryptedFilePayload(file: unknown): file is EncryptedFilePayload {
  if (!file || typeof file !== 'object') {
    return false;
  }

  const payload = file as Partial<EncryptedFilePayload>;
  return (
    isBase64(payload.fileIv) &&
    isBase64(payload.fileCiphertext) &&
    isBase64(payload.metaIv) &&
    isBase64(payload.metaCiphertext)
  );
}

export function requireEncryptedPayload(
  body: Record<string, unknown>,
): ValidatedEncryptedPayload | ValidationError {
  const lookup = readLookupKey(body);
  if ('error' in lookup) {
    return lookup;
  }

  if (Array.isArray(body.files)) {
    if (body.files.length === 0) {
      return { error: 'files must include at least one encrypted file.' };
    }
    for (let i = 0; i < body.files.length; i += 1) {
      if (!isEncryptedFilePayload(body.files[i])) {
        return {
          error: `files[${i}] must include base64 fileIv/fileCiphertext/metaIv/metaCiphertext.`,
        };
      }
    }
    return { key: lookup.key, files: body.files };
  }

  const fields = ['fileIv', 'fileCiphertext', 'metaIv', 'metaCiphertext'] as const;
  const singleFile = {} as EncryptedFilePayload;
  for (const field of fields) {
    const value = body[field];
    if (!value) {
      return { error: `${field} is required.` };
    }
    if (!isBase64(value)) {
      return { error: `${field} must be base64.` };
    }
    singleFile[field] = value;
  }

  return {
    key: lookup.key,
    files: [singleFile],
  };
}
