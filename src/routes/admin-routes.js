import { Router } from 'express';

export function createAdminRoutes(adminController) {
  const router = Router();

  router.get('/admin', adminController.page);
  router.post('/admin/reset', adminController.reset);
  router.get('/admin/stats', adminController.stats);
  router.get('/enject', adminController.legacyAdminRedirect);
  router.post('/enject/reset', adminController.legacyResetRedirect);

  return router;
}
