import { expect, test } from '@playwright/test';
import { startServer } from '../tests/helpers/server.mjs';

// figures that change or stand in a column use tabular digits; headings wrap balanced.
test.use({ locale: 'en-US' });

let server;
test.beforeAll(async () => {
  server = await startServer();
});
test.afterAll(async () => {
  await server?.stop?.();
});

test('figures are tabular, headings balanced', async ({ page }) => {
  await page.route(/\/events$/, (route) =>
    route.fulfill({ status: 200, contentType: 'text/event-stream', body: 'data: {"type":"agent_status","agent":"Planner","message":"hi"}\n\n' }));
  await page.route(/\/usage\?/, (route) =>
    route.fulfill({
      json: {
        totals: { requests: 3 }, by_provider: { openai: { requests: 1, prompt_tokens: 2, completion_tokens: 3, cost_usd: 0.1 } },
        events: [{ ts: '2026-11-22T10:00:00Z', ok: true, provider: 'openai', model: 'gpt-4o', stream: false,
                   prompt_tokens: 1234, completion_tokens: 2, duration_ms: 56, source: 'cli' }],
      },
    }));
  await page.goto(await server.dashboardUrl());
  await expect(page.locator('#usage-events .usage-event')).toHaveCount(1);
  await expect(page.locator('#log .log-ts')).not.toHaveCount(0);

  for (const selector of ['.kpi-value', '#usage-by-provider td.num', '#log .log-ts', '.usage-event .log-ts', '.usage-event .tnum']) {
    const values = await page.locator(selector).evaluateAll((els) => els.map((el) => getComputedStyle(el).fontVariantNumeric));
    expect(values.length, selector).toBeGreaterThan(0);
    for (const value of values) expect(value, selector).toContain('tabular-nums');
  }
  // the three figures of an event: tokens in, tokens out, duration
  await expect(page.locator('.usage-event .tnum')).toHaveCount(3);

  for (const selector of ['h1', '.card h2']) {
    const values = await page.locator(selector).evaluateAll((els) => els.map((el) => getComputedStyle(el).textWrapStyle));
    expect(values.length, selector).toBeGreaterThan(0);
    for (const value of values) expect(value, selector).toBe('balance');
  }
});

test('a heading that fits on one line is not changed by balancing', async ({ page }) => {
  await page.goto(await server.dashboardUrl());
  // The normal wrap stays: the heading's text is the same either way, and a one-line heading is as wide as its text.
  const h1 = page.locator('h1');
  const [box, lines] = await h1.evaluate((el) => {
    const range = document.createRange();
    range.selectNodeContents(el);
    // one rect per text run (the name of the app is a span of its own): the lines are the distinct tops
    const tops = new Set([...range.getClientRects()].map((r) => Math.round(r.top)));
    return [el.getBoundingClientRect().height, tops.size];
  });
  expect(lines).toBe(1);
  expect(box).toBeLessThan(40);
});
