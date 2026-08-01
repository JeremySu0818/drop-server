import React, { useEffect, useRef, useState } from 'react';
import { hydrateRoot } from 'react-dom/client';

import { AdminApp } from '../views/admin-app.js';

type AdminStatsResponse = {
  uploadCount: number;
  memoryUsage: string;
};

type BootstrapData = {
  uploadCount: number;
  memoryUsage: string;
};

declare global {
  interface Window {
    __ADMIN_BOOTSTRAP__?: BootstrapData;
  }
}

function parseMemory(value: string): { number: number; suffix: string } | null {
  const match = value.match(/^(\d+)(.*)$/);
  if (!match) {
    return null;
  }
  return { number: Number(match[1]), suffix: match[2] };
}

function animateValue(
  start: number,
  end: number,
  duration: number,
  onUpdate: (value: number) => void,
): number | null {
  if (start === end) {
    onUpdate(end);
    return null;
  }

  const startTime = performance.now();
  let frameId = 0;

  const update = (currentTime: number) => {
    const elapsed = currentTime - startTime;
    const progress = Math.min(elapsed / duration, 1);
    const eased = progress * (2 - progress);
    const value = Math.round(start + (end - start) * eased);
    onUpdate(value);
    if (progress < 1) {
      frameId = requestAnimationFrame(update);
    }
  };

  frameId = requestAnimationFrame(update);
  return frameId;
}

function AdminClient() {
  const bootstrap = window.__ADMIN_BOOTSTRAP__ || {
    uploadCount: 0,
    memoryUsage: '0 MB / 0 GB',
  };
  const initialMemoryParsed = parseMemory(bootstrap.memoryUsage) || {
    number: 0,
    suffix: ' MB / 0 GB',
  };

  const [uploadCount, setUploadCount] = useState(bootstrap.uploadCount);
  const [memoryUsage, setMemoryUsage] = useState(bootstrap.memoryUsage);
  const [resetDisabled, setResetDisabled] = useState(
    bootstrap.uploadCount === 0,
  );

  const uploadCountRef = useRef(bootstrap.uploadCount);
  const memoryNumberRef = useRef(initialMemoryParsed.number);
  const memorySuffixRef = useRef(initialMemoryParsed.suffix);
  const uploadAnimationRef = useRef<number | null>(null);
  const memoryAnimationRef = useRef<number | null>(null);

  useEffect(() => {
    uploadCountRef.current = uploadCount;
  }, [uploadCount]);

  useEffect(() => {
    const parsed = parseMemory(memoryUsage);
    if (parsed) {
      memoryNumberRef.current = parsed.number;
      memorySuffixRef.current = parsed.suffix;
    }
  }, [memoryUsage]);

  useEffect(() => {
    let disposeEffects = () => {};
    let disposed = false;

    (async () => {
      try {
        const liquidGlassModuleUrl =
          'https://esm.sh/solid-glass@0.0.3/engines/svg-refraction';
        const uiEffectsModuleUrl = '/static/js/ui-effects.js';
        const [{ createLiquidGlass }, { initPageEffects }] = await Promise.all([
          import(liquidGlassModuleUrl),
          import(uiEffectsModuleUrl),
        ]);
        if (disposed) {
          return;
        }
        disposeEffects = initPageEffects(createLiquidGlass);
      } catch (error) {
        console.error('Failed to initialize page effects:', error);
      }
    })();

    return () => {
      disposed = true;
      disposeEffects();
    };
  }, []);

  useEffect(() => {
    let intervalId: number | null = null;
    let eventSource: EventSource | null = null;

    const handleUpdate = (data: AdminStatsResponse) => {
      if (uploadAnimationRef.current !== null) {
        cancelAnimationFrame(uploadAnimationRef.current);
      }
      uploadAnimationRef.current = animateValue(
        uploadCountRef.current,
        data.uploadCount,
        500,
        (value) => {
          setUploadCount(value);
          uploadCountRef.current = value;
        },
      );

      const nextMemory = parseMemory(data.memoryUsage);
      if (nextMemory) {
        if (memoryAnimationRef.current !== null) {
          cancelAnimationFrame(memoryAnimationRef.current);
        }
        memorySuffixRef.current = nextMemory.suffix;
        memoryAnimationRef.current = animateValue(
          memoryNumberRef.current,
          nextMemory.number,
          500,
          (value) => {
            memoryNumberRef.current = value;
            setMemoryUsage(`${value}${memorySuffixRef.current}`);
          },
        );
      } else {
        setMemoryUsage(data.memoryUsage);
      }

      setResetDisabled(data.uploadCount === 0);
    };

    const fetchStats = async () => {
      try {
        const res = await fetch('/admin/stats');
        if (!res.ok) {
          return;
        }

        const data = (await res.json()) as AdminStatsResponse;
        handleUpdate(data);
      } catch (error) {
        console.error('Failed to update stats:', error);
      }
    };

    if (typeof EventSource !== 'undefined') {
      eventSource = new EventSource('/admin/events');

      eventSource.onmessage = (event) => {
        try {
          const data = JSON.parse(event.data) as AdminStatsResponse;
          handleUpdate(data);
        } catch (error) {
          console.error('Failed to parse SSE stats:', error);
        }
      };

      eventSource.onerror = () => {
        if (eventSource) {
          eventSource.close();
          eventSource = null;
        }
        if (intervalId === null) {
          intervalId = window.setInterval(fetchStats, 1000);
        }
      };
    } else {
      intervalId = window.setInterval(fetchStats, 1000);
    }

    return () => {
      if (eventSource) {
        eventSource.close();
      }
      if (intervalId !== null) {
        window.clearInterval(intervalId);
      }
      if (uploadAnimationRef.current !== null) {
        cancelAnimationFrame(uploadAnimationRef.current);
      }
      if (memoryAnimationRef.current !== null) {
        cancelAnimationFrame(memoryAnimationRef.current);
      }
    };
  }, []);

  return (
    <AdminApp
      uploadCount={uploadCount}
      memoryUsage={memoryUsage}
      resetDisabled={resetDisabled}
    />
  );
}

const rootElement = document.getElementById('admin-root');
if (rootElement) {
  hydrateRoot(rootElement, <AdminClient />);
}
