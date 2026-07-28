import { Router, type RequestHandler } from 'express';

type AdminController = {
  eventsStream: RequestHandler;
  page: RequestHandler;
  reset: RequestHandler;
  stats: RequestHandler;
  legacyAdminRedirect: RequestHandler;
  legacyResetRedirect: RequestHandler;
};

export function createAdminRoutes(adminController: AdminController) {
  const router = Router();

  router.get('/admin', adminController.page);
  router.post('/admin/reset', adminController.reset);
  router.get('/admin/stats', adminController.stats);
  router.get('/admin/events', adminController.eventsStream);
  router.get('/enject', adminController.legacyAdminRedirect);
  router.post('/enject/reset', adminController.legacyResetRedirect);

  return router;
}
