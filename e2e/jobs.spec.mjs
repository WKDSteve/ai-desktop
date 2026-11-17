import AxeBuilder from '@axe-core/playwright';
import { expect, test } from '@playwright/test';
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import http from 'node:http';
import os from 'node:os';
import path from 'node:path';
import { startServer } from '../tests/helpers/server.mjs';

// The jobs of the family apps in the dashboard (docs/JOBS.md), against the real binary: a prefilled job waits with its
// text and nothing runs; a job sent while the page is open is selected; Run sends the edited text; a private area asks
// before it goes to a model that is not local; a job is put away; the settings turn the jobs off. A local
// OpenAI-compatible mock plays the model.
test.use({ locale: 'en-US' });

const TASK = {
  source: 'tasks-desktop',
  task_id: 'ai-desktop/auftrag-von-aussen',
  title: 'Take a job from outside',
  markdown: '---\ntitle: Take a job\n---\n\n## Acceptance\n\n- [ ] one\n',
  vault: 'E:/repos/tasks-vault',
  area: 'ai-desktop',
  mode: 'prefill',
};

let server;
let mock;
let configDir;
const asked = [];

function startMock() {
  const srv = http.createServer((req, res) => {
    const chunks = [];
    req.on('data', (c) => chunks.push(c));
    req.on('end', () => {
      asked.push(JSON.parse(Buffer.concat(chunks).toString('utf8') || '{}'));
      res.writeHead(200, { 'Content-Type': 'text/event-stream' });
      for (const piece of ['Plan: ', 'step one']) {
        res.write(`data: ${JSON.stringify({ choices: [{ delta: { content: piece } }] })}\n\n`);
      }
      res.end('data: [DONE]\n\n');
    });
  });
  return new Promise((resolve) => srv.listen(0, '127.0.0.1', () => resolve(srv)));
}

function sendJob(body) {
  const secret = readFileSync(path.join(configDir, 'control.secret'), 'utf8').trim();
  return server.post('/jobs', body, { Authorization: `Bearer ${secret}` });
}

test.beforeAll(async () => {
  mock = await startMock();
  configDir = mkdtempSync(path.join(os.tmpdir(), 'ai-desktop-e2e-jobs-'));
  writeFileSync(
    path.join(configDir, 'config.json'),
    JSON.stringify({
      version: 1,
      settings: { private_areas: 'tricentis-vault', default_model: 'mock-local' },
      custom_models: [
        { id: 'mock-local', provider: 'openai-compatible', base_url: `http://127.0.0.1:${mock.address().port}/v1` },
      ],
    }),
  );
  server = await startServer({ configDir, env: { AI_DESKTOP_JOBS_PER_MINUTE: '1000' } });
});
test.afterAll(async () => {
  await server?.stop?.();
  await new Promise((r) => mock?.close(r));
  rmSync(configDir, { recursive: true, force: true });
});

const jobText = (page) => page.getByRole('textbox', { name: 'Text of the job' });

test('a prefilled job stands in the dashboard with its text, and nothing runs', async ({ page }) => {
  const before = asked.length;
  const { json } = await sendJob(TASK);
  await page.goto(`http://127.0.0.1:${server.port}/#job=${json.job_id}`);
  await expect(jobText(page)).toHaveValue(TASK.markdown);
  await expect(page.locator('#job-status')).toHaveText(/Prefilled/);
  await expect(page.locator('#job-meta')).toContainText('ai-desktop/auftrag-von-aussen');
  await expect(page.locator('#job-meta')).toContainText('kind handoff');
  await expect(page.getByRole('combobox', { name: 'Model of the job' })).toHaveAttribute('placeholder', /mock-local/);
  await page.waitForTimeout(300);
  expect(asked.length).toBe(before);
});

test('a job sent while the page is open is selected; Run sends the edited text', async ({ page }) => {
  await page.goto(`http://127.0.0.1:${server.port}/`);
  await expect(page.locator('#sse-status')).toHaveText('Connected');
  const { json } = await sendJob({ ...TASK, title: 'Sent while open', kind: 'draft-task' });
  await expect(page.locator(`[data-job="${json.job_id}"]`)).toHaveAttribute('aria-current', 'true');
  await expect(jobText(page)).toHaveValue(TASK.markdown);
  await expect(page.locator('#job-meta')).toContainText('kind draft-task');

  await jobText(page).fill('Edited by the person');
  await page.getByRole('button', { name: /Run/ }).click();
  await expect(page.locator('#job-output')).toHaveText('Plan: step one');
  await expect(page.locator('#job-status')).toHaveText('Done.');
  expect(asked.at(-1).messages.some((m) => m.content.includes('Edited by the person'))).toBe(true);
  await expect(page.locator(`[data-job="${json.job_id}"]`)).toContainText('done');
});

test('a private area asks before a cloud model, and is accessible while it asks', async ({ page }) => {
  const { json } = await sendJob({ ...TASK, area: 'tricentis-vault', title: 'Private one' });
  await page.goto(`http://127.0.0.1:${server.port}/#job=${json.job_id}`);
  await expect(page.locator('#job-meta')).toContainText('private');
  const before = asked.length;
  await page.getByRole('combobox', { name: 'Model of the job' }).fill('gpt-4o');
  await page.getByRole('button', { name: /Run/ }).click();
  await expect(page.locator('#job-confirm-text')).toHaveText(/tricentis-vault is private.*gpt-4o/);
  await expect(page.getByRole('button', { name: 'Send it to this model anyway' })).toBeFocused();
  for (const scheme of ['light', 'dark']) {
    await page.emulateMedia({ colorScheme: scheme, reducedMotion: 'reduce' });
    await page.waitForTimeout(400);  // the buttons fade to the new scheme (.btn transition)
    const result = await new AxeBuilder({ page })
      .withTags(['wcag2a', 'wcag2aa', 'wcag21a', 'wcag21aa', 'best-practice'])
      .analyze();
    expect(result.violations.map((v) => [v.id, v.nodes.map((n) => n.html)])).toEqual([]);
  }

  // The yes sends it; gpt-4o has no key here, so the run fails, but it went.
  await page.getByRole('button', { name: 'Send it to this model anyway' }).click();
  await expect(page.locator('#job-status')).toHaveText(/Failed/);
  await expect(page.locator('#job-confirm')).toBeHidden();

  // On the local model it runs without a question.
  await page.getByRole('combobox', { name: 'Model of the job' }).fill('mock-local');
  await page.getByRole('button', { name: /Run/ }).click();
  await expect(page.locator('#job-status')).toHaveText('Done.');
  expect(asked.length).toBe(before + 1);
});

test('a job is put away', async ({ page }) => {
  const { json } = await sendJob({ ...TASK, title: 'To put away' });
  await page.goto(`http://127.0.0.1:${server.port}/#job=${json.job_id}`);
  await expect(jobText(page)).toBeVisible();
  await page.getByRole('button', { name: 'Put away', exact: true }).click();
  await expect(page.locator(`[data-job="${json.job_id}"]`)).toHaveCount(0);
  await expect(page.locator('#job-detail')).toBeHidden();
  expect((await server.get(`/jobs/${json.job_id}`)).status).toBe(404);
});

test('the settings turn the jobs off and name the private areas', async ({ page }) => {
  await page.goto(`http://127.0.0.1:${server.port}/`);
  await page.getByText('Settings of the jobs').click();
  const accept = page.getByRole('checkbox', { name: 'Take jobs from the apps of the family' });
  await expect(accept).toBeChecked();
  await accept.uncheck();
  await expect(page.locator('#job-settings-status')).toHaveText('Saved.');
  expect((await sendJob(TASK)).status).toBe(403);

  await accept.check();
  await expect.poll(async () => (await sendJob(TASK)).status).toBe(201);

  const areas = page.getByRole('textbox', { name: 'Private areas:' });
  await expect(areas).toHaveValue('tricentis-vault');
  await areas.fill(' tricentis-vault , WKDBook-Tricentis ');
  await areas.press('Tab');
  await expect(areas).toHaveValue('tricentis-vault,WKDBook-Tricentis');
  expect((await server.get('/settings')).json.settings.private_areas.value).toBe('tricentis-vault,WKDBook-Tricentis');
});
