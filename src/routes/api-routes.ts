import { Router, type RequestHandler } from 'express';

type ApiController = {
  health: RequestHandler;
  reset: RequestHandler;
  uploads: RequestHandler;
  download: RequestHandler;
};

export function createApiRoutes(apiController: ApiController) {
  const router = Router();

  router.get('/health', apiController.health);
  router.post('/reset', apiController.reset);
  router.post('/uploads', apiController.uploads);
  router.post('/download', apiController.download);

  return router;
}
