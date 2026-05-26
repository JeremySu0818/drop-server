import { runtimeConfig } from './src/config/runtime-config.js';
import { createApp } from './src/app.js';

const app = createApp(runtimeConfig);

app.listen(runtimeConfig.port, () => {
  console.log(`PicDrop server listening on :${runtimeConfig.port}`);
});
