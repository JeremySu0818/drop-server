import assert from 'node:assert/strict';
import test from 'node:test';

import {
  CHUNK_SIZE_BYTES,
  ChunkedUploadStore,
} from '../dist/src/services/chunked-upload-store.js';

const lookupKey = 'e'.repeat(64);
const iv = Buffer.alloc(12, 4).toString('base64');
const file = {
  id: 'f0',
  size: 1,
  chunkCount: 1,
  metaIv: iv,
  metaCiphertext: Buffer.from([1]).toString('base64'),
};

const wait = (milliseconds) =>
  new Promise((resolve) => setTimeout(resolve, milliseconds));

test('chunk activity renews an in-progress upload expiry', async () => {
  const store = new ChunkedUploadStore({ ttlMs: 80 });
  const created = store.createUpload(lookupKey, [file]);
  assert.equal(created.chunkSize, CHUNK_SIZE_BYTES);

  await wait(50);
  assert.deepEqual(
    store.beginChunk(created.uploadId, file.id, 0, iv, 17),
    { ok: true },
  );
  const bytes = Buffer.alloc(17, 8);
  assert.deepEqual(
    store.commitChunk(created.uploadId, file.id, 0, iv, bytes),
    { ok: true },
  );
  await wait(50);

  const completed = store.completeUpload(created.uploadId);
  assert.equal(completed.ok, true);
  store.clearUploads();
});

test('expired and explicitly aborted chunks are zeroed and released', async () => {
  const expiredStore = new ChunkedUploadStore({ ttlMs: 10 });
  const expired = expiredStore.createUpload(lookupKey, [file]);
  expiredStore.beginChunk(expired.uploadId, file.id, 0, iv, 17);
  const expiredBytes = Buffer.alloc(17, 9);
  expiredStore.commitChunk(expired.uploadId, file.id, 0, iv, expiredBytes);
  await wait(20);
  assert.equal(expiredStore.purgeExpired(), 1);
  assert.ok(expiredBytes.every((byte) => byte === 0));

  const abortedStore = new ChunkedUploadStore({ ttlMs: 1000 });
  const aborted = abortedStore.createUpload(lookupKey, [file]);
  abortedStore.beginChunk(aborted.uploadId, file.id, 0, iv, 17);
  const abortedBytes = Buffer.alloc(17, 6);
  abortedStore.commitChunk(aborted.uploadId, file.id, 0, iv, abortedBytes);
  assert.deepEqual(abortedStore.abortUpload(aborted.uploadId), { ok: true });
  assert.ok(abortedBytes.every((byte) => byte === 0));
});
