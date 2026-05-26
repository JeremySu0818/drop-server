import crypto from 'node:crypto';
import express from 'express';
import cors from 'cors';
import helmet from 'helmet';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import fs from 'node:fs';
import os from 'node:os';

const app = express();
const __dirname = path.dirname(fileURLToPath(import.meta.url));
const PORT = Number(process.env.PORT || 7860);
const TTL_MS = Number(process.env.TTL_MS || 30 * 60 * 1000);
const ALLOWED_ORIGIN = process.env.ALLOWED_ORIGIN || '*';

const uploads = new Map();

app.use('/static', express.static(path.join(__dirname, 'public/static')));
app.use(
  helmet({
    crossOriginResourcePolicy: false,
    contentSecurityPolicy: false,
  }),
);
app.use(
  cors({
    origin:
      ALLOWED_ORIGIN === '*'
        ? '*'
        : ALLOWED_ORIGIN.split(',').map((origin) => origin.trim()),
    methods: ['GET', 'POST', 'OPTIONS'],
  }),
);

function readJsonBody(req) {
  return new Promise((resolve, reject) => {
    let raw = '';
    req.setEncoding('utf8');
    req.on('data', (chunk) => {
      raw += chunk;
    });
    req.on('end', () => {
      if (!raw) {
        resolve({});
        return;
      }
      try {
        resolve(JSON.parse(raw));
      } catch {
        reject(new Error('INVALID_JSON'));
      }
    });
    req.on('error', reject);
  });
}

function now() {
  return Date.now();
}

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

function renderAdminPage() {
  purgeExpired();

  const templatePath = path.join(__dirname, 'views/admin.html');
  let html = '';
  try {
    html = fs.readFileSync(templatePath, 'utf8');
  } catch (e) {
    return 'Template missing';
  }

  const usedMem = process.memoryUsage().rss;
  const totalMem = os.totalmem();
  const usedMB = (usedMem / 1024 / 1024).toFixed(0);
  const totalGB = (totalMem / 1024 / 1024 / 1024).toFixed(1);
  const memoryUsage = `${usedMB} MB / ${totalGB} GB`;

  html = html.replace('<!--UPLOAD_COUNT-->', uploads.size.toString());
  html = html.replace('<!--MEMORY_USAGE-->', memoryUsage);
  html = html.replace(
    '<!--RESET_DISABLED-->',
    uploads.size === 0 ? 'disabled' : '',
  );

  return html;
}

function isLookupKey(value) {
  return typeof value === 'string' && /^[a-f0-9]{64}$/i.test(value);
}

function isBase64(value) {
  return (
    typeof value === 'string' &&
    value.length > 0 &&
    /^[A-Za-z0-9+/]+={0,2}$/.test(value)
  );
}

function readLookupKey(body) {
  const key = String(body.lookupKey || body.lookupHash || '').toLowerCase();
  if (!isLookupKey(key)) {
    return { error: 'lookupKey must be a SHA-256 hex digest.' };
  }
  return { key };
}

function isEncryptedFilePayload(file) {
  if (!file || typeof file !== 'object') {
    return false;
  }
  return (
    isBase64(file.fileIv) &&
    isBase64(file.fileCiphertext) &&
    isBase64(file.metaIv) &&
    isBase64(file.metaCiphertext)
  );
}

function requireEncryptedPayload(body) {
  const lookup = readLookupKey(body);
  if (lookup.error) {
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

  const fields = ['fileIv', 'fileCiphertext', 'metaIv', 'metaCiphertext'];
  for (const field of fields) {
    if (!body[field]) {
      return { error: `${field} is required.` };
    }
    if (!isBase64(body[field])) {
      return { error: `${field} must be base64.` };
    }
  }

  return {
    key: lookup.key,
    files: [
      {
        fileIv: body.fileIv,
        fileCiphertext: body.fileCiphertext,
        metaIv: body.metaIv,
        metaCiphertext: body.metaCiphertext,
      },
    ],
  };
}

app.get('/api/health', (_req, res) => {
  purgeExpired();
  res.json({
    ok: true,
    ttlMinutes: Math.round(TTL_MS / 60000),
    stored: uploads.size,
  });
});

app.get('/admin', (req, res) => {
  res.type('html').send(renderAdminPage());
});

app.post('/admin/reset', (_req, res) => {
  clearUploads();
  res.redirect(303, '/admin');
});

app.get('/enject', (_req, res) => {
  res.redirect(301, '/admin');
});

app.post('/enject/reset', (_req, res) => {
  res.redirect(308, '/admin/reset');
});

app.post('/api/reset', (_req, res) => {
  const cleared = clearUploads();
  res.json({ ok: true, cleared, stored: uploads.size });
});

app.post('/api/uploads', async (req, res) => {
  purgeExpired();
  let body;
  try {
    body = await readJsonBody(req);
  } catch (error) {
    if (error?.message === 'INVALID_JSON') {
      res.status(400).json({ error: 'Invalid JSON body.' });
      return;
    }
    throw error;
  }
  const parsed = requireEncryptedPayload(body);
  if (parsed.error) {
    res.status(400).json({ error: parsed.error });
    return;
  }

  const createdAt = now();
  const existing = uploads.get(parsed.key);
  if (existing) {
    if (existing.expiresAt <= createdAt) {
      uploads.delete(parsed.key);
    } else {
      existing.files.push(...parsed.files);
      res.status(200).json({
        ok: true,
        expiresAt: existing.expiresAt,
        files: existing.files.length,
      });
      return;
    }
  }

  const expiresAt = createdAt + TTL_MS;
  uploads.set(parsed.key, {
    id: crypto.randomUUID(),
    files: [...parsed.files],
    createdAt,
    expiresAt,
  });

  res.status(201).json({ ok: true, expiresAt, files: parsed.files.length });
});

app.post('/api/download', async (req, res) => {
  purgeExpired();
  let body;
  try {
    body = await readJsonBody(req);
  } catch (error) {
    if (error?.message === 'INVALID_JSON') {
      res.status(400).json({ error: 'Invalid JSON body.' });
      return;
    }
    throw error;
  }
  const lookup = readLookupKey(body || {});
  if (lookup.error) {
    res.status(400).json({ error: 'lookupKey must be a SHA-256 hex digest.' });
    return;
  }

  const record = uploads.get(lookup.key);
  if (!record) {
    res.status(404).json({
      error: 'Image not found. It may have already been downloaded or expired.',
    });
    return;
  }
  uploads.delete(lookup.key);

  if (record.expiresAt <= now()) {
    res.status(410).json({ error: 'Image has expired.' });
    return;
  }

  res.json({
    files: record.files,
  });
});

app.use((_req, res) => {
  res.status(404).json({ error: 'Not found.' });
});

app.use((error, _req, res, _next) => {
  res.status(500).json({ error: 'Internal server error.' });
});

setInterval(purgeExpired, Math.min(TTL_MS, 60 * 1000)).unref();

app.listen(PORT, () => {
  console.log(`PicDrop server listening on :${PORT}`);
});
