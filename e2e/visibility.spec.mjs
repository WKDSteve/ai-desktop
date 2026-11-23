import { expect, test } from '@playwright/test';
import { startServer } from '../tests/helpers/server.mjs';

// the poll of a running job (700 ms) and the reconnect of the event stream (3 s) rest while the tab is hidden and
// catch up once when it is shown again.
test.use({ locale: 'en-US' });

let server;
test.beforeAll(async () => {
  server = await startServer();
});
test.afterAll(async () => {
  await server?.stop?.();
});

const JOB_ID = '0123456789abcdef';

async function setHidden(page, hidden) {
  await page.evaluate((value) => {
    Object.defineProperty(document, 'hidden', { configurable: true, get: () => value });
    document.dispatchEvent(new Event('visibilitychange'));
  }, hidden);
}

test('a hidden tab does not poll a running job and reads it once when shown again', async ({ page }) => {
  let reads = 0;
  const job = {
    id: JOB_ID, title: 'T', source: 'tasks-desktop', kind: 'plan', status: 'running', task_id: 'x/y', area: 'x',
    markdown: 'm', model: '', output: '', runner: 'model', private: false,
  };
  await page.route(/\/jobs$/, (route) => route.fulfill({ json: { jobs: [job] } }));
  await page.route(new RegExp(`/jobs/${JOB_ID}$`), (route) => {
    reads += 1;
    return route.fulfill({ json: job });
  });
  await page.route(/\/events$/, (route) => route.fulfill({ status: 200, contentType: 'text/event-stream', body: '' }));
  await page.goto((await server.dashboardUrl()).replace(/#.*/, '') + `#job=${JOB_ID}`);
  await expect.poll(() => reads).toBeGreaterThan(1); // polling while visible

  await setHidden(page, true);
  await page.waitForTimeout(1000); // a timer already pending may fire once and park itself
  const whileHidden = reads;
  await page.waitForTimeout(2500);
  expect(reads).toBe(whileHidden);

  await setHidden(page, false);
  await expect.poll(() => reads).toBeGreaterThan(whileHidden);
});

test('a hidden tab does not reconnect the event stream and connects once when shown again', async ({ page }) => {
  let connects = 0;
  await page.route(/\/events$/, (route) => {
    connects += 1;
    return route.fulfill({ status: 500, body: 'no' });
  });
  await page.goto(await server.dashboardUrl());
  await expect.poll(() => connects).toBe(1);

  await setHidden(page, true);
  await page.waitForTimeout(3600); // longer than the 3 s beat
  expect(connects).toBe(1);

  await setHidden(page, false);
  await expect.poll(() => connects).toBe(2);
});
