import React from 'react';
import { renderToString } from 'react-dom/server';

import { AdminApp, type AdminAppProps } from './admin-app.js';

type AdminBootstrap = {
  uploadCount: number;
  memoryUsage: string;
};

function escapeForScript(value: string): string {
  return value.replace(/</g, '\\u003c');
}

export function renderAdminPage(props: AdminAppProps): string {
  const appHtml = renderToString(<AdminApp {...props} />);
  const bootstrap: AdminBootstrap = {
    uploadCount: props.uploadCount,
    memoryUsage: props.memoryUsage,
  };
  const bootstrapJson = escapeForScript(JSON.stringify(bootstrap));

  return `<!doctype html>
<html lang="en">
  <head>
    <meta charset="utf-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1" />
    <meta name="color-scheme" content="dark" />
    <title>PicDrop Admin</title>
    <link rel="icon" href="/static/favicon.ico" sizes="any" />
    <link rel="preconnect" href="https://fonts.googleapis.com" />
    <link rel="preconnect" href="https://fonts.gstatic.com" crossorigin />
    <link
      href="https://fonts.googleapis.com/css2?family=Outfit:wght@500;700;800&display=swap"
      rel="stylesheet"
    />
    <link rel="preconnect" href="https://esm.sh" />
    <link rel="stylesheet" href="https://esm.sh/solid-glass@0.0.3/css" />
    <link rel="stylesheet" href="./static/css/style.css" />
    <style>
      .primary-action:disabled {
        cursor: not-allowed !important;
      }
    </style>
  </head>
  <body>
    <div id="admin-root">${appHtml}</div>
    <script>window.__ADMIN_BOOTSTRAP__ = ${bootstrapJson};</script>
    <script type="module" src="/static/js/admin.js"></script>
  </body>
</html>
`;
}
