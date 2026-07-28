import { Router, type RequestHandler } from 'express';

type ApiController = {
  health: RequestHandler;
  reset: RequestHandler;
  uploads: RequestHandler;
  download: RequestHandler;
};

type ChunkedApiController = {
  createUpload: RequestHandler;
  uploadChunk: RequestHandler;
  completeUpload: RequestHandler;
  abortUpload: RequestHandler;
  beginDownload: RequestHandler;
  downloadChunk: RequestHandler;
  finishDownload: RequestHandler;
};

export function createApiRoutes(
  apiController: ApiController,
  chunkedApiController: ChunkedApiController,
) {
  const router = Router();

  router.get('/health', apiController.health);
  router.post('/reset', apiController.reset);
  router.post('/uploads', apiController.uploads);
  router.post('/download', apiController.download);
  router.post('/chunked-uploads', chunkedApiController.createUpload);
  router.put(
    '/chunked-uploads/:uploadId/files/:fileId/chunks/:index',
    chunkedApiController.uploadChunk,
  );
  router.post(
    '/chunked-uploads/complete',
    chunkedApiController.completeUpload,
  );
  router.delete(
    '/chunked-uploads/:uploadId',
    chunkedApiController.abortUpload,
  );
  router.post(
    '/chunked-uploads/:uploadId/abort',
    chunkedApiController.abortUpload,
  );
  router.post('/chunked-download', chunkedApiController.beginDownload);
  router.get(
    '/chunked-download/:downloadId/files/:fileId/chunks/:index',
    chunkedApiController.downloadChunk,
  );
  router.delete(
    '/chunked-download/:downloadId',
    chunkedApiController.finishDownload,
  );

  return router;
}
