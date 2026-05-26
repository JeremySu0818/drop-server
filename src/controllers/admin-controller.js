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

  return {
    legacyAdminRedirect,
    legacyResetRedirect,
    page,
    reset,
  };
}
