import { expect, test } from '@playwright/test';
import { startServer } from '../tests/helpers/server.mjs';

// a text field is 1rem or more (a smaller one makes a phone's browser zoom in when it gets the focus), and a
// field for a model id or an area name is not spell-checked.
test.use({ locale: 'en-US' });

let server;
test.beforeAll(async () => {
  server = await startServer();
});
test.afterAll(async () => {
  await server?.stop?.();
});

test('every field of the page is at least 1rem', async ({ page }) => {
  await page.goto(await server.dashboardUrl());
  await page.locator('#job-detail').evaluate((el) => (el.hidden = false)); // its fields are laid out too
  await page.locator('#job-settings').evaluate((el) => (el.open = true));
  const sizes = await page.evaluate(() => {
    const root = parseFloat(getComputedStyle(document.documentElement).fontSize);
    return [...document.querySelectorAll('textarea, select, input:not([type=checkbox]):not([type=radio])')].map((el) => ({
      id: el.id, rem: parseFloat(getComputedStyle(el).fontSize) / root,
    }));
  });
  expect(sizes.map((s) => s.id)).toEqual(
    expect.arrayContaining(['ask-prompt', 'ask-model', 'job-markdown', 'job-model', 'private-areas', 'usage-provider', 'usage-range']),
  );
  for (const { id, rem } of sizes) expect(rem, `#${id}`).toBeGreaterThanOrEqual(1);
});

test('the fields of ids and names are not spell-checked', async ({ page }) => {
  await page.goto(await server.dashboardUrl());
  for (const id of ['ask-model', 'job-model', 'private-areas']) {
    expect(await page.locator(`#${id}`).evaluate((el) => el.spellcheck), `#${id}`).toBe(false);
  }
  // prose stays checked
  expect(await page.locator('#ask-prompt').evaluate((el) => el.spellcheck)).toBe(true);
});
