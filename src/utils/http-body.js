export function readJsonBody(req) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    req.setEncoding('utf8');
    req.on('data', (chunk) => {
      chunks.push(chunk);
    });
    req.on('end', () => {
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
        resolve(JSON.parse(raw));
      } catch {
        reject(new Error('INVALID_JSON'));
      }
    });
    req.on('error', reject);
  });
}
