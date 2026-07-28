import type { Request } from 'express';

export const INVALID_JSON = 'INVALID_JSON';
export const PAYLOAD_TOO_LARGE = 'PAYLOAD_TOO_LARGE';

export function readJsonBody(
  req: Request,
  maxBytes: number,
): Promise<Record<string, unknown>> {
  return new Promise((resolve, reject) => {
    const declaredLength = Number(req.headers['content-length']);
    if (Number.isFinite(declaredLength) && declaredLength > maxBytes) {
      req.resume();
      reject(new Error(PAYLOAD_TOO_LARGE));
      return;
    }

    const chunks: string[] = [];
    let receivedBytes = 0;
    let tooLarge = false;

    req.setEncoding('utf8');
    req.on('data', (chunk: string) => {
      if (tooLarge) {
        return;
      }
      receivedBytes += Buffer.byteLength(chunk, 'utf8');
      if (receivedBytes > maxBytes) {
        tooLarge = true;
        chunks.length = 0;
        return;
      }
      chunks.push(chunk);
    });
    req.on('end', () => {
      if (tooLarge) {
        reject(new Error(PAYLOAD_TOO_LARGE));
        return;
      }
      if (chunks.length === 0) {
        resolve({});
        return;
      }

      const raw = chunks.length === 1 ? chunks[0] : chunks.join('');
      if (!raw) {
        resolve({});
        return;
      }
      try {
        const parsed = JSON.parse(raw) as unknown;
        if (parsed && typeof parsed === 'object' && !Array.isArray(parsed)) {
          resolve(parsed as Record<string, unknown>);
          return;
        }
        resolve({});
      } catch {
        reject(new Error(INVALID_JSON));
      }
    });
    req.on('error', reject);
  });
}
