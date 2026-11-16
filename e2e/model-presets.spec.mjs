import { expect, test } from '@playwright/test';
import { startServer } from '../tests/helpers/server.mjs';

// The suggestions of the model field of Ask come from GET /models (the models the server knows), not from a fixed
// list in index.html that ages with every model release. They are asked for when the field gets the focus the first
// time: opening the dashboard does not make /models list the providers.
test.use({ locale: 'en-US' });

let server;
test.beforeAll(async () => {
  server = await startServer();
});
test.afterAll(async () => {
  await server?.stop?.();
});

const LISTING = {
  default: 'llama3.1:8b',
  models: [
    { id: 'llama3.1:8b', category: 'local', available: true },
    { id: 'gpt-5-mini', category: 'cloud', available: false },
    { id: 'claude-haiku-5-5', category: 'cloud', available: true },
  ],
  errors: [],
};

const options = (page) =>
  page.locator('#ask-model-presets option').evaluateAll((list) => list.map((o) => o.value));

test('the field has no fixed suggestion and does not ask before it gets the focus', async ({ page }) => {
  let asked = 0;
  await page.route('**/models', (route) => {
    asked += 1;
    return route.fulfill({ json: LISTING });
  });
  await page.goto(`http://127.0.0.1:${server.port}/`);
  await expect(page.locator('#backends-status')).not.toHaveText(/loading/);
  expect(await options(page)).toEqual([]);
  expect(asked).toBe(0);
});

test('the first focus fills the suggestions from /models, once', async ({ page }) => {
  let asked = 0;
  await page.route('**/models', (route) => {
    asked += 1;
    return route.fulfill({ json: LISTING });
  });
  await page.goto(`http://127.0.0.1:${server.port}/`);
  const field = page.getByRole('combobox', { name: 'Model' });
  await field.focus();
  await expect.poll(() => options(page)).toEqual(['llama3.1:8b', 'gpt-5-mini', 'claude-haiku-5-5']);
  await field.blur();
  await field.focus();
  await page.waitForTimeout(200);
  expect(asked).toBe(1);
});

test('without an answer of /models the field stays free text', async ({ page }) => {
  await page.route('**/models', (route) => route.abort());
  await page.goto(`http://127.0.0.1:${server.port}/`);
  const field = page.getByRole('combobox', { name: 'Model' });
  await field.focus();
  await field.fill('gemini-2.5-flash');
  await expect(field).toHaveValue('gemini-2.5-flash');
  expect(await options(page)).toEqual([]);
});
