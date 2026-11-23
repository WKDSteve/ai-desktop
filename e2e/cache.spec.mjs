import { expect, test } from '@playwright/test';
import { startServer } from '../tests/helpers/server.mjs';

// The dashboard files come with a validator: a second load asks the server and is answered with 304 (not a full 200), and
// the page still works from the copy.
test.use({ locale: 'en-US' });

let server;
test.beforeAll(async () => {
  server = await startServer();
});
test.afterAll(async () => {
  await server?.stop?.();
});

test('a reload revalidates the dashboard files (304) and the page still works', async ({ page }) => {
  const url = await server.dashboardUrl();
  await page.goto(url);
  await expect(page.locator('#sse-status')).toHaveText('Connected');

  const answers = {};
  page.on('response', (res) => {
    const name = new URL(res.url()).pathname;
    if (['/app.js', '/style.css', '/i18n.js', '/palette.css'].includes(name)) answers[name] = res.status();
  });
  await page.reload();
  await expect(page.locator('#sse-status')).toHaveText('Connected');
  expect(answers).toEqual({ '/app.js': 304, '/style.css': 304, '/i18n.js': 304, '/palette.css': 304 });
});
