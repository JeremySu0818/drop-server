import React from 'react';

export type AdminAppProps = {
  uploadCount: number;
  memoryUsage: string;
  resetDisabled: boolean;
};

export function AdminApp({ uploadCount, memoryUsage, resetDisabled }: AdminAppProps) {
  return (
    <main className="app-shell">
      <section className="brand-strip" aria-label="Drop">
        <div>
          <p className="eyebrow">Drop Admin</p>
          <h1>System Dashboard</h1>
        </div>
      </section>

      <article
        className="glass-card"
        data-liquid-glass={true}
        style={{ maxWidth: '100%', margin: '0 auto', padding: '40px' }}
      >
        <div className="card-head" style={{ marginBottom: '32px' }}>
          <span className="icon-badge" aria-hidden="true">
            <svg viewBox="0 0 24 24" role="img">
              <path d="M4 7v10c0 2 1.5 3 3.5 3h9c2 0 3.5-1 3.5-3V7c0-2-1.5-3-3.5-3h-9C5.5 4 4 5 4 7z"></path>
              <path d="M4 10h16M4 14h16"></path>
            </svg>
          </span>
          <div>
            <h2>Server Status</h2>
            <p>Real-time metrics and system controls.</p>
          </div>
        </div>

        <div
          style={{
            display: 'grid',
            gridTemplateColumns: '1fr 1fr',
            gap: '20px',
            marginBottom: '32px',
          }}
        >
          <div className="drop-zone" style={{ cursor: 'default', minHeight: 'auto', padding: '32px 24px' }}>
            <span className="drop-title" style={{ fontSize: '3rem' }} id="upload-count">
              {uploadCount}
            </span>
            <span className="drop-meta">Active Sessions</span>
          </div>

          <div className="drop-zone" style={{ cursor: 'default', minHeight: 'auto', padding: '32px 24px' }}>
            <span className="drop-title" style={{ fontSize: '2.5rem' }} id="memory-usage">
              {memoryUsage}
            </span>
            <span className="drop-meta">System Memory</span>
          </div>
        </div>

        <form method="post" action="/admin/reset" style={{ margin: 0, padding: 0 }}>
          <button
            className="primary-action"
            type="submit"
            id="reset-btn"
            style={{ background: '#ff4a4a', color: '#ffffff' }}
            disabled={resetDisabled}
          >
            <svg viewBox="0 0 24 24" aria-hidden="true">
              <path d="M19 7l-.867 12.142A2 2 0 0116.138 21H7.862a2 2 0 01-1.995-1.858L5 7m5 4v6m4-6v6m1-10V4a1 1 0 00-1-1h-4a1 1 0 00-1 1v3M4 7h16" />
            </svg>
            Clear All Data
          </button>
        </form>
      </article>
    </main>
  );
}
