import os from 'node:os';
import type { RequestHandler } from 'express';

import type { UploadStore } from '../services/upload-store.js';
import type { ChunkedUploadStore } from '../services/chunked-upload-store.js';
import { renderAdminPage } from '../views/admin-document.js';

type AdminControllerDependencies = {
  uploadStore: UploadStore;
  chunkedUploadStore: ChunkedUploadStore;
};

function getMemoryUsageLabel(): string {
  const usedMem = process.memoryUsage().rss;
  const totalMem = os.totalmem();
  const usedMb = (usedMem / 1024 / 1024).toFixed(0);
  const totalGb = (totalMem / 1024 / 1024 / 1024).toFixed(1);
  return `${usedMb} MB / ${totalGb} GB`;
}

export function createAdminController({
  uploadStore,
  chunkedUploadStore,
}: AdminControllerDependencies) {
  const getUploadCount = () =>
    uploadStore.getStats().uploadCount +
    chunkedUploadStore.getStats().uploadCount;

  const page: RequestHandler = (_req, res) => {
    const uploadCount = getUploadCount();
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
    chunkedUploadStore.clearUploads();
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
      uploadCount: getUploadCount(),
      memoryUsage: getMemoryUsageLabel(),
    });
  };

  return {
    legacyAdminRedirect,
    legacyResetRedirect,
    page,
    reset,
    stats,
  };
}
