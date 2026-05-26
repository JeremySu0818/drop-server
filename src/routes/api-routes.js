import { Router } from 'express';

export function createApiRoutes(apiController) {
  const router = Router();

  router.get('/health', apiController.health);
  router.post('/reset', apiController.reset);
  router.post('/uploads', apiController.uploads);
  router.post('/download', apiController.download);

  return router;
}
