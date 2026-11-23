import { expect, test } from '@playwright/test';
import { startServer } from '../tests/helpers/server.mjs';

// names, identifiers and paths carry translate="no" (effective value, so a marked parent counts), the words
// around them stay translatable.
test.use({ locale: 'en-US' });

let server;
test.beforeAll(async () => {
  server = await startServer();
});
test.afterAll(async () => {
  await server?.stop?.();
});

const JOB_ID = '0123456789abcdef';

// The effective translate value of the innermost element whose own text is exactly `text`.
function translateOf(page, text, selector = '*') {
  return page.evaluate(
    ([wanted, scope]) => {
      const hits = [...document.querySelectorAll(scope)].filter(
        (el) => el.childElementCount === 0 && el.textContent.trim() === wanted,
      );
      return hits.map((el) => el.translate);
    },
    [text, selector],
  );
}

test('names, identifiers and paths are marked translate=no, the words around them are not', async ({ page }) => {
  const job = {
    id: JOB_ID, title: 'Plan it', source: 'tasks-desktop', kind: 'plan', status: 'running', task_id: 'ai-desktop/a-task',
    area: 'ai-desktop', markdown: 'm', model: 'gpt-4o', output: '', runner: 'model', private: false,
  };
  await page.route(/\/health$/, (route) =>
    route.fulfill({ json: { backends: { ollama: true, openai: true, anthropic: false, gemini: false } } }));
  await page.route(/\/jobs$/, (route) => route.fulfill({ json: { jobs: [job] } }));
  await page.route(new RegExp(`/jobs/${JOB_ID}$`), (route) => route.fulfill({ json: job }));
  await page.route(/\/events$/, (route) => route.fulfill({ status: 200, contentType: 'text/event-stream', body: '' }));
  await page.route(/\/usage\?/, (route) =>
    route.fulfill({
      json: {
        totals: {}, by_provider: { openai: { requests: 1, prompt_tokens: 2, completion_tokens: 3, cost_usd: 0.1 } },
        events: [{ ts: '2026-11-22T10:00:00Z', ok: true, provider: 'openai', model: 'gpt-4o-mini', stream: false,
                   prompt_tokens: 1, completion_tokens: 2, duration_ms: 5, source: 'ai-nvim' }],
      },
    }));
  await page.goto((await server.dashboardUrl()).replace(/#.*/, '') + `#job=${JOB_ID}`);
  await expect(page.locator('#job-meta')).toContainText('ai-desktop/a-task');
  await expect(page.locator('#usage-events .usage-event')).toHaveCount(1);
  await expect(page.locator('#backends-status span')).toHaveCount(4);

  // index.html
  for (const text of ['gpt-', 'o1-', 'o3-', 'claude-', 'gemini-']) {
    expect(await translateOf(page, text, '#ask-hint code')).toEqual([false]);
  }
  for (const text of ['OpenAI', 'Anthropic', 'Gemini']) {
    expect(await translateOf(page, text, '#ask-hint span')).toEqual([false]);
  }
  expect(await page.locator('#sse-badge').evaluate((el) => el.translate)).toBe(false);
  expect(await page.locator('#backends-badge').evaluate((el) => el.translate)).toBe(false);
  // the words around stay translatable
  expect(await page.locator('#ask-hint').evaluate((el) => el.translate)).toBe(true);
  expect(await page.locator('#jobs-card h2').evaluate((el) => el.translate)).toBe(true);

  // written by app.js
  expect(await page.locator('#backends-status span').evaluateAll((els) => els.map((el) => el.translate)))
    .toEqual([false, false, false, false]);
  expect(await translateOf(page, 'ai-desktop/a-task', '#job-meta *')).toEqual([false]);
  expect(await translateOf(page, 'ai-desktop', '#job-meta *')).toEqual([false]);
  expect(await translateOf(page, 'gpt-4o', '#job-runner *')).toEqual([false]);
  expect(await page.locator('#job-meta').evaluate((el) => el.translate)).toBe(true);
  expect(await translateOf(page, 'openai', '#usage-by-provider td')).toEqual([false]);
  expect(await translateOf(page, 'openai', '#usage-events *')).toEqual([false]);
  expect(await translateOf(page, 'gpt-4o-mini', '#usage-events *')).toEqual([false]);
  expect(await translateOf(page, 'ai-nvim', '#usage-events *')).toEqual([false]);
  expect(await translateOf(page, 'openai', '#usage-provider option')).toEqual([false]);
  expect(await translateOf(page, 'tasks-desktop', '#jobs-list *')).toEqual([false]);
});
