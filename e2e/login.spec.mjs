import AxeBuilder from '@axe-core/playwright';
import { expect, test } from '@playwright/test';
import { startServer } from '../tests/helpers/server.mjs';

// The login of the dashboard (src/session.hpp): a ticket in the address becomes a session token in localStorage, and
// the ticket leaves the address at once; without one the page says how to log in.
let server;
test.beforeAll(async () => {
  server = await startServer();
});
test.afterAll(async () => {
  await server?.stop?.();
});
test.use({ locale: 'en-US' });

test('without a ticket the page says how to log in, and reads no settings', async ({ page }) => {
  await page.goto(`http://127.0.0.1:${server.port}/`);
  const banner = page.getByRole('alert').filter({ hasText: 'Not logged in' });
  await expect(banner).toBeVisible();
  await expect(banner).toContainText('#ticket=');
  await expect(page.locator('#sse-status')).not.toHaveText('Connected');
  const results = await new AxeBuilder({ page }).include('#login-banner').analyze();
  expect(results.violations).toEqual([]);
});

test('a ticket logs the page in and leaves the address; the token outlives a reload', async ({ page }) => {
  const created = await server.post('/jobs', {
    source: 'tasks-desktop', task_id: 'demo/login', title: 'Login job', markdown: 'text\n', area: 'demo',
    vault: 'E:/repos/tasks-vault', mode: 'prefill',
  });
  const id = created.json.job_id;
  await page.goto(await server.dashboardUrl(`job=${id}`));
  await expect(page.locator('#sse-status')).toHaveText('Connected');
  await expect(page.locator('#login-banner')).toBeHidden();
  expect(new URL(page.url()).hash).toBe(`#job=${id}`);
  await expect(page.locator(`[data-job="${id}"]`)).toHaveAttribute('aria-current', 'true');
  const token = await page.evaluate(() => localStorage.getItem('ai-desktop.session'));
  expect(token).toMatch(/^[0-9a-f]{64}$/);

  await page.reload();
  await expect(page.locator('#sse-status')).toHaveText('Connected');
  await expect(page.locator('#login-banner')).toBeHidden();
});

test('a new ticket for a page with a token of an earlier start reads everything anew', async ({ page }) => {
  expect((await server.put('/settings', { private_areas: 'login-area' })).status).toBe(200);
  await page.addInitScript(() => {
    if (!sessionStorage.getItem('seeded')) {
      sessionStorage.setItem('seeded', '1');
      localStorage.setItem('ai-desktop.session', '0'.repeat(64));
    }
  });
  await page.goto(`http://127.0.0.1:${server.port}/`);
  await expect(page.getByRole('alert').filter({ hasText: 'Not logged in' })).toBeVisible();
  await page.goto(await server.dashboardUrl());
  await expect(page.locator('#login-banner')).toBeHidden();
  await expect(page.locator('#private-areas')).toHaveValue('login-area');
  await expect(page.locator('#sse-status')).toHaveText('Connected');
  await server.put('/settings', { private_areas: null });
});

test('a used ticket does not log in again', async ({ page }) => {
  const url = await server.dashboardUrl();
  const ticket = new URL(url).hash.slice('#ticket='.length);
  expect((await server.post('/session', { ticket }, { Authorization: undefined })).status).toBe(200);
  await page.goto(url);
  await expect(page.getByRole('alert').filter({ hasText: 'Not logged in' })).toBeVisible();
});
