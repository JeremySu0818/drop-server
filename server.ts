import path from 'node:path';
import { fileURLToPath } from 'node:url';

import { runtimeConfig } from './src/config/runtime-config.js';
import { createApp } from './src/app.js';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const rootDir = path.basename(__dirname) === 'dist'
  ? path.resolve(__dirname, '..')
  : __dirname;

const app = createApp({ config: runtimeConfig, rootDir });

app.listen(runtimeConfig.port, () => {
  console.log(`PicDrop server listening on :${runtimeConfig.port}`);
});
