import { expect, test } from '@playwright/test';
import { startServer } from '../tests/helpers/server.mjs';

// WCAG 2.5.8: a control is a target of 24 x 24 px at least.
test.use({ locale: 'en-US' });

let server;
test.beforeAll(async () => {
  server = await startServer();
});
test.afterAll(async () => {
  await server?.stop?.();
});

test('the x of the provider choice and the summary of the job settings are 24 px targets', async ({ page }) => {
  await page.route(/\/usage\?/, (route) =>
    route.fulfill({ json: { totals: {}, by_provider: { openai: { requests: 1 } }, events: [] } }));
  await page.goto(await server.dashboardUrl());
  await expect(page.locator('#usage-provider option[value="openai"]')).toHaveCount(1);
  await page.locator('#usage-provider').selectOption('openai');
  const clear = page.locator('#usage-provider-clear');
  await expect(clear).toBeVisible();
  const x = await clear.boundingBox();
  expect(x.width).toBeGreaterThanOrEqual(24);
  expect(x.height).toBeGreaterThanOrEqual(24);
  // the x sits inside the field, and the field's own text keeps clear of it
  const field = await page.locator('#usage-provider').boundingBox();
  expect(x.x).toBeGreaterThanOrEqual(field.x);
  expect(x.x + x.width).toBeLessThanOrEqual(field.x + field.width);
  const padding = await page.locator('#usage-provider').evaluate((el) => parseFloat(getComputedStyle(el).paddingRight));
  expect(field.x + field.width - padding).toBeLessThanOrEqual(x.x + 1);

  const summary = await page.locator('#job-settings summary').boundingBox();
  expect(summary.width).toBeGreaterThanOrEqual(24);
  expect(summary.height).toBeGreaterThanOrEqual(24);
});
