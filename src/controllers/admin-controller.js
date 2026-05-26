import os from 'node:os';
import { renderAdminPage } from '../services/admin-page.js';

export function createAdminController({ rootDir, uploadStore }) {
  function page(_req, res) {
    uploadStore.purgeExpired();
    res.type('html').send(
      renderAdminPage({
        rootDir,
        uploadCount: uploadStore.size(),
      }),
    );
  }

  function reset(_req, res) {
    uploadStore.clearUploads();
    res.redirect(303, '/admin');
  }

  function legacyAdminRedirect(_req, res) {
    res.redirect(301, '/admin');
  }

  function legacyResetRedirect(_req, res) {
    res.redirect(308, '/admin/reset');
  }

  function stats(_req, res) {
    const usedMem = process.memoryUsage().rss;
    const totalMem = os.totalmem();
    const usedMb = (usedMem / 1024 / 1024).toFixed(0);
    const totalGb = (totalMem / 1024 / 1024 / 1024).toFixed(1);
    
    res.json({
      uploadCount: uploadStore.size(),
      memoryUsage: `${usedMb} MB / ${totalGb} GB`
    });
  }

  return {
    legacyAdminRedirect,
    legacyResetRedirect,
    page,
    reset,
    stats,
  };
}
