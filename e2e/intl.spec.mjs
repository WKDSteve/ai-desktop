import { expect, test } from '@playwright/test';
import { readFileSync } from 'node:fs';
import { repoRoot, startServer } from '../tests/helpers/server.mjs';

// times, dates and the unit of a duration come from Intl in the language of the page; nothing forces the clock
// (hour12) or writes the unit as text.
test.use({ timezoneId: 'UTC' });

let server;
test.beforeAll(async () => {
  server = await startServer();
});
test.afterAll(async () => {
  await server?.stop?.();
});

async function open(page) {
  await page.clock.setFixedTime(new Date('2026-11-22T00:05:07Z'));
  await page.route(/\/events$/, (route) => route.fulfill({ status: 200, contentType: 'text/event-stream', body: '' }));
  await page.route(/\/usage\?/, (route) =>
    route.fulfill({
      json: {
        totals: {}, by_provider: {},
        events: [{ ts: '2026-11-22T10:00:00Z', ok: true, provider: 'openai', model: 'gpt-4o', stream: false,
                   prompt_tokens: 1, completion_tokens: 2, duration_ms: 1234, source: 'cli' }],
      },
    }));
  await page.goto(await server.dashboardUrl());
  await expect(page.locator('#usage-events .usage-event')).toHaveCount(1);
  await expect(page.locator('#log .log-ts').first()).toBeVisible();
}

const CASES = [
  { locale: 'de-AT', date: '22.11.2026, 10:00:00', duration: /^1\s234\sms$/ },
  { locale: 'en-GB', date: '22/11/2026, 10:00:00', duration: /^1,234\sms$/ },
];
for (const c of CASES) {
  test.describe(c.locale, () => {
    test.use({ locale: c.locale });
    test('time, date and duration in the form of the language', async ({ page }) => {
      await open(page);
      await expect(page.locator('#log .log-ts').first()).toHaveText('00:05:07'); // the hour after midnight is 00, never 24
      await expect(page.locator('#usage-events .log-ts')).toHaveText(c.date);
      await expect(page.locator('#usage-events .tnum').last()).toHaveText(c.duration);
    });
  });
}

test('the script does not force the clock or write the unit of a duration as text', () => {
  // the code, not its comments (a comment may say what is not done)
  const source = readFileSync(`${repoRoot}/dashboard/app.js`, 'utf8').replace(/^\s*\/\/.*$/gm, '');
  expect(source).not.toMatch(/hour12/);
  expect(source).not.toMatch(/['"`] ms['"`]/);
  expect(source).toMatch(/style: 'unit'/);
});
