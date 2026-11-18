import AxeBuilder from '@axe-core/playwright';
import { expect, test } from '@playwright/test';
import { copyFileSync, existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import http from 'node:http';
import os from 'node:os';
import path from 'node:path';
import { binaryPath, startServer } from '../tests/helpers/server.mjs';

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
      settings: { private_areas: 'tricentis-vault', default_model: 'mock-local', jobs_model: 'mock-local' },
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
  await page.goto(await server.dashboardUrl(`job=${json.job_id}`));
  await expect(jobText(page)).toHaveValue(TASK.markdown);
  await expect(page.locator('#job-status')).toHaveText(/Prefilled/);
  await expect(page.locator('#job-meta')).toContainText('ai-desktop/auftrag-von-aussen');
  await expect(page.locator('#job-meta')).toContainText('kind handoff');
  await expect(page.getByRole('combobox', { name: 'Model of the job' })).toHaveAttribute('placeholder', /mock-local/);
  await page.waitForTimeout(300);
  expect(asked.length).toBe(before);
});

test('a job sent while the page is open is selected; Run sends the edited text', async ({ page }) => {
  await page.goto(await server.dashboardUrl());
  await expect(page.locator('#sse-status')).toHaveText('Connected');
  const { json } = await sendJob({ ...TASK, title: 'Sent while open', kind: 'review-done' });
  await expect(page.locator(`[data-job="${json.job_id}"]`)).toHaveAttribute('aria-current', 'true');
  await expect(jobText(page)).toHaveValue(TASK.markdown);
  await expect(page.locator('#job-meta')).toContainText('kind review-done');

  await jobText(page).fill('Edited by the person');
  await page.getByRole('button', { name: /Run/ }).click();
  await expect(page.locator('#job-output')).toHaveText('Plan: step one');
  await expect(page.locator('#job-status')).toHaveText('Done.');
  expect(asked.at(-1).messages.some((m) => m.content.includes('Edited by the person'))).toBe(true);
  await expect(page.locator(`[data-job="${json.job_id}"]`)).toContainText('done');
});

test('a private area asks before a cloud model, and is accessible while it asks', async ({ page }) => {
  const { json } = await sendJob({ ...TASK, area: 'tricentis-vault', title: 'Private one' });
  await page.goto(await server.dashboardUrl(`job=${json.job_id}`));
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
  await page.goto(await server.dashboardUrl(`job=${json.job_id}`));
  await expect(jobText(page)).toBeVisible();
  await page.getByRole('button', { name: 'Put away', exact: true }).click();
  await expect(page.locator(`[data-job="${json.job_id}"]`)).toHaveCount(0);
  await expect(page.locator('#job-detail')).toBeHidden();
  expect((await server.get(`/jobs/${json.job_id}`)).status).toBe(404);
});

test('the settings turn the jobs off and name the private areas', async ({ page }) => {
  await page.goto(await server.dashboardUrl());
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

test('a job of claude-code shows how it runs, and Stop ends it', async ({ page }) => {
  // The stand-ins of tests/doubles: fake_claude claims, then waits (a child of it waits too) until it is stopped.
  const exe = process.platform === 'win32' ? '.exe' : '';
  const build = path.dirname(binaryPath());
  test.skip(!existsSync(path.join(build, `fake_claude${exe}`)), 'build the test doubles first');
  const root = mkdtempSync(path.join(os.tmpdir(), 'ai-desktop-e2e-agent-'));
  const bin = path.join(root, 'bin');
  mkdirSync(bin);
  copyFileSync(path.join(build, `fake_tasks${exe}`), path.join(bin, `tasks${exe}`));
  mkdirSync(path.join(root, 'vault'));
  const agentServer = await startServer({
    env: {
      AI_DESKTOP_CLAUDE_CLI: path.join(build, `fake_claude${exe}`),
      AI_DESKTOP_TASKS_CLI: path.join(bin, `tasks${exe}`),
      FAKE_CLAUDE_SLEEP_MS: '30000',
      FAKE_TASKS_LOG: path.join(root, 'tasks.log'),
    },
  });
  try {
    const secret = readFileSync(path.join(agentServer.configDir, 'control.secret'), 'utf8').trim();
    const { json } = await agentServer.post('/jobs', { ...TASK, vault: path.join(root, 'vault'), mode: 'run' },
      { Authorization: `Bearer ${secret}` });
    await page.goto(await agentServer.dashboardUrl(`job=${json.job_id}`));
    await expect(page.locator('#job-runner')).toHaveText(
      'Claude Code with the brief of the tasks family (brief 1, contract 1, tasks.nvim fake-0)');
    await expect(page.locator('#job-output')).toContainText('Bash: tasks claim ai-desktop/auftrag-von-aussen');
    await expect(page.locator('#job-status')).toHaveText('Running...');
    await expect(page.locator('#job-stop')).toBeEnabled();
    await page.locator('#job-stop').click();
    await expect(page.locator('#job-status')).toHaveText('Cancelled.');
    await expect(page.locator('#job-stop')).toBeDisabled();
    await expect(page.getByRole('button', { name: /Run/ })).toBeEnabled();
  } finally {
    await agentServer.stop();
    rmSync(root, { recursive: true, force: true });
  }
});

// Answers that come after the person started typing do not overwrite what they typed (the windows job of the CI met
// both races).
test('the settings arriving late keep what was typed', async ({ page }) => {
  await page.route('**/settings', async (route) => {
    if (route.request().method() === 'GET') await new Promise((r) => setTimeout(r, 800));
    await route.continue();
  });
  await page.goto(await server.dashboardUrl());
  await page.getByText('Settings of the jobs').click();
  const areas = page.getByRole('textbox', { name: 'Private areas:' });
  await areas.fill('tricentis-vault,typed-early');
  await areas.press('Tab');
  await page.waitForTimeout(1200);
  await expect(areas).toHaveValue('tricentis-vault,typed-early');
  // Only the field that changed is saved: the switch of the jobs, not arrived yet, stays on.
  expect((await server.get('/settings')).json.settings.jobs.value).toBe('on');
  await page.unroute('**/settings');
  await server.put('/settings', { private_areas: 'tricentis-vault' });
});

test('the shown job selected again keeps the model that was typed', async ({ page }) => {
  const { json } = await sendJob({ ...TASK, title: 'Keep my model' });
  await page.goto(await server.dashboardUrl(`job=${json.job_id}`));
  await expect(page.locator('#job-meta')).toContainText('ai-desktop/auftrag-von-aussen');
  const model = page.getByRole('combobox', { name: 'Model of the job' });
  await model.fill('gpt-4o');
  await jobText(page).fill('typed text');
  // What the event of the job or a second look at the address does: select it again.
  await page.evaluate(() => window.dispatchEvent(new HashChangeEvent('hashchange')));
  await page.waitForTimeout(500);
  await expect(model).toHaveValue('gpt-4o');
  await expect(jobText(page)).toHaveValue('typed text');
});
