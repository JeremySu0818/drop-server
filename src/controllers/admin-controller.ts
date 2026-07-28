import os from 'node:os';
import type { RequestHandler } from 'express';

import type { UploadStore } from '../services/upload-store.js';
import { renderAdminPage } from '../views/admin-document.js';

type AdminControllerDependencies = {
  uploadStore: UploadStore;
};

function getMemoryUsageLabel(): string {
  const usedMem = process.memoryUsage().rss;
  const totalMem = os.totalmem();
  const usedMb = (usedMem / 1024 / 1024).toFixed(0);
  const totalGb = (totalMem / 1024 / 1024 / 1024).toFixed(1);
  return `${usedMb} MB / ${totalGb} GB`;
}

export function createAdminController({ uploadStore }: AdminControllerDependencies) {
  const page: RequestHandler = (_req, res) => {
    const uploadCount = uploadStore.getStats().uploadCount;
    res.type('html').send(
      renderAdminPage({
        uploadCount,
        memoryUsage: getMemoryUsageLabel(),
        resetDisabled: uploadCount === 0,
      }),
    );
  };

  const reset: RequestHandler = (_req, res) => {
    uploadStore.clearUploads();
    res.redirect(303, '/admin');
  };

  const legacyAdminRedirect: RequestHandler = (_req, res) => {
    res.redirect(301, '/admin');
  };

  const legacyResetRedirect: RequestHandler = (_req, res) => {
    res.redirect(308, '/admin/reset');
  };

  const stats: RequestHandler = (_req, res) => {
    res.json({
      uploadCount: uploadStore.getStats().uploadCount,
      memoryUsage: getMemoryUsageLabel(),
    });
  };

  const eventsStream: RequestHandler = (req, res) => {
    res.setHeader('Content-Type', 'text/event-stream');
    res.setHeader('Cache-Control', 'no-cache, no-transform');
    res.setHeader('Connection', 'keep-alive');
    res.setHeader('X-Accel-Buffering', 'no');

    const sendStats = () => {
      const data = {
        uploadCount: uploadStore.getStats().uploadCount,
        memoryUsage: getMemoryUsageLabel(),
      };
      res.write(`data: ${JSON.stringify(data)}\n\n`);
    };

    sendStats();

    const timer = setInterval(sendStats, 1000);

    req.on('close', () => {
      clearInterval(timer);
    });
  };

  return {
    eventsStream,
    legacyAdminRedirect,
    legacyResetRedirect,
    page,
    reset,
    stats,
  };
}
