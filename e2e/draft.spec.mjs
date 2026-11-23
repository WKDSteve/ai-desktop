import { expect, test } from '@playwright/test';
import { startServer } from '../tests/helpers/server.mjs';

// the prompt typed and not sent survives a reload (only that text, in localStorage); an edited job text asks
// before a reload takes it.
test.use({ locale: 'en-US' });

let server;
test.beforeAll(async () => {
  server = await startServer();
});
test.afterAll(async () => {
  await server?.stop?.();
});

const JOB_ID = '0123456789abcdef';

test('the prompt not yet sent is still there after a reload, and goes when the answer is in', async ({ page }) => {
  await page.route(/\/ask\/stream$/, (route) =>
    route.fulfill({ status: 200, contentType: 'text/event-stream', body: 'data: {"delta":"the answer"}\n\ndata: [DONE]\n\n' }));
  const url = await server.dashboardUrl();
  await page.goto(url);
  await page.locator('#ask-prompt').fill('draft: what is 6 times 7?');
  await page.reload();
  await expect(page.locator('#ask-prompt')).toHaveValue('draft: what is 6 times 7?');

  // only the draft text is kept: no answer, nothing else under this origin but the token
  await page.getByRole('button', { name: /Send/ }).click();
  await expect(page.locator('#ask-output')).toHaveText('the answer');
  await expect(page.locator('#ask-status')).toHaveText('Done.');
  const stored = await page.evaluate(() => Object.fromEntries(Object.entries(localStorage)));
  expect(JSON.stringify(stored)).not.toContain('the answer');
  await page.reload();
  await expect(page.locator('#ask-prompt')).toHaveValue('');
});

test('a failed request keeps the draft', async ({ page }) => {
  await page.route(/\/ask\/stream$/, (route) => route.fulfill({ status: 500, body: 'no' }));
  await page.goto(await server.dashboardUrl());
  await page.locator('#ask-prompt').fill('keep me');
  await page.getByRole('button', { name: /Send/ }).click();
  await expect(page.locator('#ask-status')).toContainText('HTTP 500');
  await page.reload();
  await expect(page.locator('#ask-prompt')).toHaveValue('keep me');
});

test('an emptied field leaves no draft', async ({ page }) => {
  await page.goto(await server.dashboardUrl());
  await page.locator('#ask-prompt').fill('x');
  await page.locator('#ask-prompt').fill('');
  expect(await page.evaluate(() => localStorage.getItem('ai-desktop.draft.ask'))).toBeNull();
});

test('a reload with an edited job text asks first; an unchanged one does not', async ({ page }) => {
  const job = {
    id: JOB_ID, title: 'T', source: 'tasks-desktop', kind: 'plan', status: 'prefilled', task_id: 'x/y', area: 'x',
    markdown: 'the text of the task', model: '', output: '', runner: 'model', private: false,
  };
  await page.route(/\/jobs$/, (route) => route.fulfill({ json: { jobs: [job] } }));
  await page.route(new RegExp(`/jobs/${JOB_ID}$`), (route) => route.fulfill({ json: job }));
  await page.goto((await server.dashboardUrl()).replace(/#.*/, '') + `#job=${JOB_ID}`);
  await expect(page.locator('#job-markdown')).toHaveValue('the text of the task');
  const asks = () => page.evaluate(() => {
    const event = new Event('beforeunload', { cancelable: true });
    window.dispatchEvent(event);
    return event.defaultPrevented;
  });
  expect(await asks()).toBe(false);
  await page.locator('#job-markdown').fill('the text of the task, edited');
  expect(await asks()).toBe(true);
  // the text of a job is never copied into the storage
  const stored = await page.evaluate(() => JSON.stringify(Object.entries(localStorage)));
  expect(stored).not.toContain('edited');
  await page.locator('#job-markdown').fill('the text of the task');
  expect(await asks()).toBe(false);
});
