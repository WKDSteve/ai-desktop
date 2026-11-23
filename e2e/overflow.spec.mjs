import { expect, test } from '@playwright/test';
import { startServer } from '../tests/helpers/server.mjs';

// text from outside with no break opportunity (a long id, a path) breaks inside its box and does not widen the page,
// and an empty list says so.
test.use({ locale: 'en-US', viewport: { width: 700, height: 800 } });

let server;
test.beforeAll(async () => {
  server = await startServer();
});
test.afterAll(async () => {
  await server?.stop?.();
});

const JOB_ID = '0123456789abcdef';
const LONG = 'x'.repeat(300);

async function mockPage(page, { events, usageEvents, providers, jobs }) {
  await page.route(/\/events$/, (route) =>
    route.fulfill({ status: 200, contentType: 'text/event-stream', body: events.map((e) => `data: ${JSON.stringify(e)}\n\n`).join('') }));
  await page.route(/\/usage\?/, (route) =>
    route.fulfill({ json: { totals: {}, by_provider: providers, events: usageEvents } }));
  await page.route(/\/jobs$/, (route) => route.fulfill({ json: { jobs } }));
  for (const job of jobs) {
    await page.route(new RegExp(`/jobs/${job.id}$`), (route) => route.fulfill({ json: job }));
  }
}

test('long unbreakable text stays inside the page', async ({ page }) => {
  const job = {
    id: JOB_ID, title: 'T', source: 'tasks-desktop', kind: 'plan', status: 'needs_confirmation', task_id: LONG, area: LONG,
    markdown: 'm', model: LONG, output: '', runner: 'model', private: true,
  };
  await mockPage(page, {
    events: [
      { type: 'agent_status', agent: 'Planner', message: LONG },
      { type: 'decision_point', agent: 'Executor', message: LONG, ts: 'a' },
    ],
    providers: { [LONG]: { requests: 1, prompt_tokens: 2, completion_tokens: 3, cost_usd: 0.1 } },
    usageEvents: [{ ts: '2026-11-22T10:00:00Z', ok: true, provider: LONG, model: LONG, stream: false,
                    prompt_tokens: 1, completion_tokens: 2, duration_ms: 5, source: LONG }],
    jobs: [job],
  });
  await page.goto((await server.dashboardUrl()).replace(/#.*/, '') + `#job=${JOB_ID}`);
  await expect(page.locator('#decision-banner')).toBeVisible();
  await expect(page.locator('#job-confirm-text')).toContainText(LONG);
  await expect(page.locator('#usage-events .usage-event')).toHaveCount(1);
  await expect(page.locator('#log .log-line')).not.toHaveCount(0);

  const overflow = await page.evaluate(() => {
    const wide = (el) => el.scrollWidth - el.clientWidth;
    const probes = ['#msg-Planner', '#decision-text', '#job-meta', '#job-confirm-text', '#log .log-msg',
                    '#usage-events .usage-event', '#usage-by-provider td'];
    return {
      page: document.documentElement.scrollWidth - window.innerWidth,
      boxes: probes.map((sel) => [sel, wide(document.querySelector(sel))]),
      cards: [...document.querySelectorAll('.card, #decision-banner')].map((el) => wide(el)),
      // what reaches past the right edge of the window (for the failure message)
      past: [...document.querySelectorAll('body *')]
        .filter((el) => el.getBoundingClientRect().right > window.innerWidth + 1)
        .map((el) => el.tagName.toLowerCase() + (el.id ? '#' + el.id : '') + (el.className ? '.' + el.className : '')),
    };
  });
  expect(overflow.page, overflow.past.join(' ')).toBeLessThanOrEqual(0);
  for (const [sel, extra] of overflow.boxes) expect(extra, sel).toBeLessThanOrEqual(0);
  for (const extra of overflow.cards) expect(extra).toBeLessThanOrEqual(0);
});

test('a failed job status with a long error stays inside its box', async ({ page }) => {
  const job = {
    id: JOB_ID, title: 'T', source: 'tasks-desktop', kind: 'plan', status: 'failed', error: LONG, task_id: 'a/b', area: 'a',
    markdown: 'm', model: '', output: '', runner: 'model', private: false,
  };
  await mockPage(page, { events: [], providers: {}, usageEvents: [], jobs: [job] });
  await page.goto((await server.dashboardUrl()).replace(/#.*/, '') + `#job=${JOB_ID}`);
  await expect(page.locator('#job-status')).toContainText(LONG);
  const extra = await page.locator('#job-status').evaluate((el) => el.scrollWidth - el.clientWidth);
  expect(extra).toBeLessThanOrEqual(0);
  expect(await page.evaluate(() => document.documentElement.scrollWidth - window.innerWidth)).toBeLessThanOrEqual(0);
});

test('an empty usage list and an empty log say so', async ({ page }) => {
  await mockPage(page, { events: [], providers: {}, usageEvents: [], jobs: [] });
  await page.route(/\/events$/, (route) => route.fulfill({ status: 500, body: 'no' })); // no line in the log
  await page.goto(await server.dashboardUrl());
  await expect(page.locator('#usage-events')).toHaveText('No events in the chosen period.');
  await expect(page.locator('#log')).toContainText('No entries yet.');
  await expect(page.locator('#log-empty')).toBeVisible();
});

test('the log hides its empty note with the first line', async ({ page }) => {
  await mockPage(page, { events: [{ type: 'agent_status', agent: 'Planner', message: 'hello' }], providers: {}, usageEvents: [], jobs: [] });
  await page.goto(await server.dashboardUrl());
  await expect(page.locator('#log-empty')).toHaveCount(1); // the note exists (a missing element would be "hidden" too)
  await expect(page.locator('#log .log-line')).not.toHaveCount(0);
  await expect(page.locator('#log-empty')).toBeHidden();
});
