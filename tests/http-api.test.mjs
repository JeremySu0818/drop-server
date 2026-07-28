import assert from 'node:assert/strict';
import test from 'node:test';

import { createApp } from '../dist/src/app.js';

const rootDir = process.cwd();
const key = 'c'.repeat(64);
const file = {
  fileIv: Buffer.from([1, 2, 3]).toString('base64'),
  fileCiphertext: Buffer.from([4, 5, 6, 7]).toString('base64'),
  metaIv: Buffer.from([8, 9, 10]).toString('base64'),
  metaCiphertext: Buffer.from([11, 12]).toString('base64'),
};

async function withServer(config, callback) {
  const app = createApp({ config, rootDir });
  const server = app.listen(0);
  await new Promise((resolve) => server.once('listening', resolve));
  const address = server.address();
  const baseUrl = `http://127.0.0.1:${address.port}`;
  try {
    await callback(baseUrl);
  } finally {
    await new Promise((resolve, reject) => {
      server.close((error) => (error ? reject(error) : resolve()));
    });
  }
}

const baseConfig = {
  port: 7860,
  ttlMs: 60_000,
  maxJsonBytes: 1024,
  maxStoreBytes: 1024,
  allowedOrigin: '*',
};

test('HTTP API stores, reports, and takes encrypted files once', async () => {
  await withServer(baseConfig, async (baseUrl) => {
    const upload = await fetch(`${baseUrl}/api/uploads`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ lookupKey: key, files: [file] }),
    });
    assert.equal(upload.status, 201);
    const uploadBody = await upload.json();
    assert.equal(uploadBody.ok, true);
    assert.equal(uploadBody.files, 1);
    assert.equal(typeof uploadBody.expiresAt, 'number');

    const append = await fetch(`${baseUrl}/api/uploads`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ lookupHash: key, ...file }),
    });
    assert.equal(append.status, 200);
    assert.equal((await append.json()).files, 2);

    const health = await fetch(`${baseUrl}/api/health`);
    assert.deepEqual(await health.json(), {
      ok: true,
      ttlMinutes: 1,
      stored: 1,
    });

    const adminStats = await fetch(`${baseUrl}/admin/stats`);
    const adminStatsBody = await adminStats.json();
    assert.deepEqual(Object.keys(adminStatsBody).sort(), [
      'memoryUsage',
      'uploadCount',
    ]);
    assert.equal(adminStatsBody.uploadCount, 1);
    assert.match(adminStatsBody.memoryUsage, /^\d+ MB \/ \d+\.\d GB$/);

    const download = await fetch(`${baseUrl}/api/download`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ lookupHash: key }),
    });
    assert.equal(download.status, 200);
    assert.deepEqual(await download.json(), { files: [file, file] });

    const secondDownload = await fetch(`${baseUrl}/api/download`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ lookupKey: key }),
    });
    assert.equal(secondDownload.status, 404);
  });
});

test('HTTP API enforces JSON and native store capacity limits', async () => {
  await withServer(
    { ...baseConfig, maxStoreBytes: 11 },
    async (baseUrl) => {
      const capacityResponse = await fetch(`${baseUrl}/api/uploads`, {
        method: 'POST',
        headers: { 'content-type': 'application/json' },
        body: JSON.stringify({ lookupKey: key, files: [file] }),
      });
      assert.equal(capacityResponse.status, 507);

      const oversizedResponse = await fetch(`${baseUrl}/api/uploads`, {
        method: 'POST',
        headers: { 'content-type': 'application/json' },
        body: JSON.stringify({ padding: 'x'.repeat(2048) }),
      });
      assert.equal(oversizedResponse.status, 413);
    },
  );
});
