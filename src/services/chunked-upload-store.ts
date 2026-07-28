import { randomBytes } from 'node:crypto';

export const CHUNK_SIZE_BYTES = 64 * 1024 * 1024;
const GCM_TAG_BYTES = 16;

export type ChunkedFileInit = {
  id: string;
  size: number;
  chunkCount: number;
  metaIv: string;
  metaCiphertext: string;
};

type StoredChunk = {
  iv: string;
  size: number;
  bytes: Buffer;
};

type StoredFile = ChunkedFileInit & {
  chunks: Map<number, StoredChunk>;
  inFlight: Set<number>;
};

type SessionState = 'uploading' | 'ready' | 'downloading';

type ChunkedSession = {
  id: string;
  lookupKey: string;
  expiresAt: number;
  state: SessionState;
  downloadToken?: string;
  files: Map<string, StoredFile>;
};

type StoreOptions = {
  ttlMs: number;
};

type StoreError = {
  status: number;
  error: string;
};

type StoreResult<T> = T | StoreError;

function isStoreError(value: unknown): value is StoreError {
  return Boolean(
    value &&
      typeof value === 'object' &&
      'status' in value &&
      'error' in value,
  );
}

function expectedChunkSize(file: StoredFile, index: number): number {
  const offset = index * CHUNK_SIZE_BYTES;
  const plaintextBytes = Math.max(
    0,
    Math.min(CHUNK_SIZE_BYTES, file.size - offset),
  );
  return plaintextBytes + GCM_TAG_BYTES;
}

function makeSecret(): string {
  return randomBytes(32).toString('hex');
}

export class ChunkedUploadStore {
  readonly ttlMs: number;

  private sessions = new Map<string, ChunkedSession>();
  private lookup = new Map<string, string>();

  constructor({ ttlMs }: StoreOptions) {
    this.ttlMs = ttlMs;
  }

  getStats() {
    return {
      uploadCount: this.sessions.size,
    };
  }

  createUpload(
    lookupKey: string,
    files: ChunkedFileInit[],
  ): StoreResult<{ uploadId: string; expiresAt: number; chunkSize: number }> {
    this.purgeExpired();
    if (this.lookup.has(lookupKey)) {
      return { status: 409, error: 'An upload already exists for this code.' };
    }

    const id = makeSecret();
    const session: ChunkedSession = {
      id,
      lookupKey,
      expiresAt: Date.now() + this.ttlMs,
      state: 'uploading',
      files: new Map(
        files.map((file) => [
          file.id,
          { ...file, chunks: new Map(), inFlight: new Set() },
        ]),
      ),
    };
    this.sessions.set(id, session);
    this.lookup.set(lookupKey, id);
    return {
      uploadId: id,
      expiresAt: session.expiresAt,
      chunkSize: CHUNK_SIZE_BYTES,
    };
  }

  beginChunk(
    uploadId: string,
    fileId: string,
    index: number,
    iv: string,
    contentLength: number,
  ): StoreResult<{ ok: true }> {
    const session = this.sessions.get(uploadId);
    if (!session || session.expiresAt <= Date.now()) {
      if (session) this.destroySession(session);
      return { status: 404, error: 'Upload session not found or expired.' };
    }
    if (session.state !== 'uploading') {
      return { status: 409, error: 'Upload session is no longer writable.' };
    }
    const file = session.files.get(fileId);
    if (!file || !Number.isInteger(index) || index < 0 || index >= file.chunkCount) {
      return { status: 400, error: 'Invalid file chunk.' };
    }
    if (
      file.chunks.has(index) ||
      file.inFlight.has(index)
    ) {
      return { status: 409, error: 'This chunk was already uploaded.' };
    }
    if (contentLength !== expectedChunkSize(file, index)) {
      return { status: 400, error: 'Encrypted chunk size is invalid.' };
    }

    this.touchSession(session);
    file.inFlight.add(index);
    return { ok: true };
  }

  commitChunk(
    uploadId: string,
    fileId: string,
    index: number,
    iv: string,
    bytes: Buffer,
  ): StoreResult<{ ok: true }> {
    const session = this.sessions.get(uploadId);
    const file = session?.files.get(fileId);
    if (!session || !file || !file.inFlight.has(index)) {
      bytes.fill(0);
      return { status: 404, error: 'Upload session not found or expired.' };
    }

    file.inFlight.delete(index);
    file.chunks.set(index, { iv, size: bytes.byteLength, bytes });
    this.touchSession(session);
    return { ok: true };
  }

  failChunk(uploadId: string, fileId: string, index: number) {
    this.sessions.get(uploadId)?.files.get(fileId)?.inFlight.delete(index);
  }

  completeUpload(uploadId: string): StoreResult<{ ok: true; expiresAt: number }> {
    const session = this.sessions.get(uploadId);
    if (!session || session.expiresAt <= Date.now()) {
      if (session) this.destroySession(session);
      return { status: 404, error: 'Upload session not found or expired.' };
    }
    if (session.state !== 'uploading') {
      return { status: 409, error: 'Upload was already completed.' };
    }
    for (const file of session.files.values()) {
      if (file.inFlight.size > 0 || file.chunks.size !== file.chunkCount) {
        return { status: 409, error: 'Not all file chunks were uploaded.' };
      }
    }
    session.state = 'ready';
    this.touchSession(session);
    return { ok: true, expiresAt: session.expiresAt };
  }

  beginDownload(lookupKey: string): StoreResult<{
    downloadId: string;
    downloadToken: string;
    files: ChunkedFileInit[];
  }> {
    this.purgeExpired();
    const sessionId = this.lookup.get(lookupKey);
    const session = sessionId ? this.sessions.get(sessionId) : undefined;
    if (
      !session ||
      (session.state !== 'ready' && session.state !== 'downloading')
    ) {
      return { status: 404, error: 'Upload not found, expired, or already claimed.' };
    }

    if (session.state === 'ready') {
      session.state = 'downloading';
      session.downloadToken = makeSecret();
    }
    this.touchSession(session);
    return {
      downloadId: session.id,
      downloadToken: session.downloadToken!,
      files: [...session.files.values()].map(
        ({ id, size, chunkCount, metaIv, metaCiphertext }) => ({
          id,
          size,
          chunkCount,
          metaIv,
          metaCiphertext,
        }),
      ),
    };
  }

  getChunk(
    downloadId: string,
    token: string,
    fileId: string,
    index: number,
  ): StoreResult<StoredChunk> {
    const session = this.sessions.get(downloadId);
    if (
      !session ||
      session.expiresAt <= Date.now() ||
      session.state !== 'downloading' ||
      session.downloadToken !== token
    ) {
      if (session && session.expiresAt <= Date.now()) this.destroySession(session);
      return { status: 404, error: 'Download session not found or expired.' };
    }
    const chunk = session.files.get(fileId)?.chunks.get(index);
    if (!chunk) {
      return { status: 404, error: 'File chunk not found.' };
    }
    this.touchSession(session);
    return chunk;
  }

  finishDownload(
    downloadId: string,
    token: string,
  ): StoreResult<{ ok: true }> {
    const session = this.sessions.get(downloadId);
    if (
      !session ||
      session.state !== 'downloading' ||
      session.downloadToken !== token
    ) {
      return { status: 404, error: 'Download session not found or expired.' };
    }
    this.destroySession(session);
    return { ok: true };
  }

  abortUpload(uploadId: string): StoreResult<{ ok: true }> {
    const session = this.sessions.get(uploadId);
    if (!session || session.state !== 'uploading') {
      return { status: 404, error: 'Upload session not found.' };
    }
    this.destroySession(session);
    return { ok: true };
  }

  purgeExpired = (): number => {
    let purged = 0;
    const now = Date.now();
    for (const session of this.sessions.values()) {
      if (session.expiresAt <= now) {
        this.destroySession(session);
        purged += 1;
      }
    }
    return purged;
  };

  clearUploads = (): number => {
    const count = this.sessions.size;
    for (const session of [...this.sessions.values()]) {
      this.destroySession(session);
    }
    return count;
  };

  private destroySession(session: ChunkedSession) {
    if (!this.sessions.delete(session.id)) return;
    this.lookup.delete(session.lookupKey);
    for (const file of session.files.values()) {
      for (const chunk of file.chunks.values()) {
        chunk.bytes.fill(0);
      }
      file.chunks.clear();
    }
  }

  private touchSession(session: ChunkedSession) {
    session.expiresAt = Date.now() + this.ttlMs;
  }
}

export { isStoreError };
