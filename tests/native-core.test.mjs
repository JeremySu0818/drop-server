import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import test from 'node:test';

const require = createRequire(import.meta.url);
const { createUploadStore } = require('../build/Release/drop_core.node');

const key = 'a'.repeat(64);
const file = {
  fileIv: Buffer.from([1, 2, 3]).toString('base64'),
  fileCiphertext: Buffer.from([4, 5, 6, 7]).toString('base64'),
  metaIv: Buffer.from([8, 9, 10]).toString('base64'),
  metaCiphertext: Buffer.from([11, 12]).toString('base64'),
};
const fileBytes = 12;

test('native core appends, reports stats, and atomically takes an upload', () => {
  const store = createUploadStore(60_000);

  const inserted = store.upsertUpload(key, [file]);
  assert.equal(inserted.status, 201);
  assert.equal(inserted.payload.files, 1);

  const appended = store.upsertUpload(key, [file]);
  assert.equal(appended.status, 200);
  assert.equal(appended.payload.files, 2);
  assert.deepEqual(store.getStats(), {
    uploadCount: 1,
    fileCount: 2,
    encryptedBytes: fileBytes * 2,
  });

  const downloaded = store.takeDownload(key);
  assert.equal(downloaded.status, 200);
  assert.deepEqual(downloaded.payload.files, [file, file]);
  assert.equal(store.takeDownload(key).status, 404);
  assert.equal(store.getStats().encryptedBytes, 0);
});

test('native core removes and distinguishes an expired upload', async () => {
  const store = createUploadStore(5);
  store.upsertUpload(key, [file]);
  await new Promise((resolve) => setTimeout(resolve, 15));

  assert.equal(store.takeDownload(key).status, 410);
  assert.equal(store.getStats().encryptedBytes, 0);
});

test('native core clears all retained uploads and validates base64', () => {
  const store = createUploadStore(60_000);
  store.upsertUpload(key, [file]);
  store.upsertUpload('b'.repeat(64), [file]);
  assert.equal(store.clearUploads(), 2);
  assert.equal(store.getStats().uploadCount, 0);

  assert.throws(
    () => store.upsertUpload(key, [{ ...file, fileIv: 'A' }]),
    /valid base64/,
  );
});
