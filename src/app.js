import express from 'express';
import cors from 'cors';
import helmet from 'helmet';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

import { runtimeConfig, getPurgeInterval } from './config/runtime-config.js';
import { createAdminController } from './controllers/admin-controller.js';
import { createApiController } from './controllers/api-controller.js';
import { createAdminRoutes } from './routes/admin-routes.js';
import { createApiRoutes } from './routes/api-routes.js';
import { createUploadStore } from './services/upload-store.js';
import { now } from './utils/time.js';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const rootDir = path.resolve(__dirname, '..');

export function createApp(config = runtimeConfig) {
  const app = express();
  const uploadStore = createUploadStore({ ttlMs: config.ttlMs, now });
  const adminController = createAdminController({ rootDir, uploadStore });
  const apiController = createApiController({
    uploadStore,
    ttlMs: config.ttlMs,
  });

  app.use('/static', express.static(path.join(rootDir, 'public/static')));
  app.use(
    helmet({
      crossOriginResourcePolicy: false,
      contentSecurityPolicy: false,
    }),
  );
  app.use(
    cors({
      origin:
        config.allowedOrigin === '*'
          ? '*'
          : config.allowedOrigin.split(',').map((origin) => origin.trim()),
      methods: ['GET', 'POST', 'OPTIONS'],
    }),
  );

  app.use('/', createAdminRoutes(adminController));
  app.use('/api', createApiRoutes(apiController));

  app.use((_req, res) => {
    res.status(404).json({ error: 'Not found.' });
  });

  app.use((_error, _req, res, _next) => {
    res.status(500).json({ error: 'Internal server error.' });
  });

  setInterval(uploadStore.purgeExpired, getPurgeInterval(config.ttlMs)).unref();

  return app;
}
