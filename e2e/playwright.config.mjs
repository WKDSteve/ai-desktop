import { defineConfig, devices } from '@playwright/test';

// One browser: the dashboard runs in the desktop app's WebView2 (Chromium) and in the user's browser. The system's
// Chrome is used (`channel`), as in hub-desktop, so nothing is downloaded.
export default defineConfig({
  testDir: '.',
  timeout: 30_000,
  workers: 1,
  reporter: process.env.CI ? 'line' : 'list',
  use: { ...devices['Desktop Chrome'], channel: 'chrome', viewport: { width: 1280, height: 900 } },
});
