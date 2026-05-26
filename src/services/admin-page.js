import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

export function renderAdminPage({ rootDir, uploadCount }) {
  const templatePath = path.join(rootDir, 'src/views/admin.html');
  let html = '';
  try {
    html = fs.readFileSync(templatePath, 'utf8');
  } catch {
    return 'Template missing';
  }

  const usedMem = process.memoryUsage().rss;
  const totalMem = os.totalmem();
  const usedMb = (usedMem / 1024 / 1024).toFixed(0);
  const totalGb = (totalMem / 1024 / 1024 / 1024).toFixed(1);
  const memoryUsage = `${usedMb} MB / ${totalGb} GB`;

  html = html.replace('<!--UPLOAD_COUNT-->', uploadCount.toString());
  html = html.replace('<!--MEMORY_USAGE-->', memoryUsage);
  html = html.replace('<!--RESET_DISABLED-->', uploadCount === 0 ? 'disabled' : '');

  return html;
}
