import AxeBuilder from '@axe-core/playwright';
import { expect, test } from '@playwright/test';
import { startServer } from '../tests/helpers/server.mjs';

// The card "System" shows only what the page has checked: the SSE connection and the backends of /health. The rows
// Podman ("rootless · WSL2") and GPU ("RTX 4070 Super · CUDA 13") of the prototype said the same on every machine and
// looked like measurements on a screenshot; ai-desktop starts no process (docs/PROCESSES.md), so it cannot ask
// podman or nvidia-smi, and the rows are gone. A badge is green only after its check passed: neutral while it runs,
// red when it failed.
test.use({ locale: 'en-US' });

let server;
test.beforeAll(async () => {
  server = await startServer();
});
test.afterAll(async () => {
  await server?.stop?.();
});

const card = (page) => page.locator('.card', { has: page.getByRole('heading', { name: 'System' }) });
const badge = (page, name) => card(page).locator('.badge', { hasText: name });

test('the card shows no fixed value of the prototype', async ({ page }) => {
  await page.goto(await server.dashboardUrl());
  await expect(page.locator('#backends-status')).not.toHaveText(/loading/);
  const text = await card(page).innerText();
  expect(text).not.toMatch(/Podman|rootless|WSL2|GPU|RTX|CUDA/);
});

test('the badges turn green once their check passed', async ({ page }) => {
  await page.goto(await server.dashboardUrl());
  await expect(badge(page, 'SSE')).toHaveClass(/badge-ok/);
  await expect(badge(page, 'Backends')).toHaveClass(/badge-ok/);
});

for (const colorScheme of ['light', 'dark']) {
  test(`a badge is not green before or without its check (${colorScheme})`, async ({ page }) => {
    await page.emulateMedia({ colorScheme });
    // /health and /events never answer: both checks are still running.
    await page.route('**/health', () => {});
    await page.route('**/events', () => {});
    await page.goto(await server.dashboardUrl());
    await expect(badge(page, 'SSE')).toHaveClass(/badge-idle/);
    await expect(badge(page, 'Backends')).toHaveClass(/badge-idle/);
    await expect(card(page).locator('.badge-ok')).toHaveCount(0);
    // The neutral badge keeps its contrast in both modes.
    const result = await new AxeBuilder({ page })
      .include('.card')
      .withTags(['wcag2a', 'wcag2aa', 'wcag21a', 'wcag21aa'])
      .analyze();
    expect(result.violations.map((v) => v.id)).toEqual([]);
  });
}

test('a failed check turns its badge red', async ({ page }) => {
  await page.route('**/health', (route) => route.abort());
  await page.route('**/events', (route) => route.abort());
  await page.goto(await server.dashboardUrl());
  await expect(badge(page, 'Backends')).toHaveClass(/badge-err/);
  await expect(page.locator('#backends-status')).toHaveText('not reachable');
  await expect(badge(page, 'SSE')).toHaveClass(/badge-err/);
});

for (const colorScheme of ['light', 'dark']) {
  test(`a backend whose listing failed shows a warning and the reason, not a tick (${colorScheme})`, async ({ page }) => {
    // The key is present (/health), the provider rejected it when the models were listed: the 401 a stale key in the
    // credential store gave on 2026-11-18, the same text the server keeps (with the key part already replaced).
    await page.emulateMedia({ colorScheme });
    await page.route('**/health', (route) => route.fulfill({ json: {
      status: 'ready', backends: { ollama: true, openai: true, anthropic: true, gemini: false },
      catalog_errors: {
        anthropic: 'anthropic: HTTP 401: invalid x-api-key',
        openai: 'openai: HTTP 401: Incorrect API key provided: <key>. You can find your API key at ...',
      },
    } }));
    await page.goto(await server.dashboardUrl());
    await expect(badge(page, 'Backends')).toHaveClass(/badge-warn/);
    await expect(page.locator('#backends-status')).toContainText('Anthropic ⚠');
    await expect(page.locator('#backends-status')).toContainText('OpenAI ⚠');
    await expect(page.locator('#backends-status')).toContainText('Gemini ✗');
    await expect(page.locator('#backends-errors')).toBeVisible();
    await expect(page.locator('#backends-errors')).toContainText('Anthropic: key rejected (invalid or revoked)');
    await expect(page.locator('#backends-errors')).not.toContainText('sk-');
    // The warning colour keeps its contrast in the card (the other cards have checks of their own).
    const result = await new AxeBuilder({ page })
      .include('.card:has(#backends-status)')
      .withTags(['wcag2a', 'wcag2aa', 'wcag21a', 'wcag21aa'])
      .analyze();
    const found = result.violations.map((v) => `${v.id}: ${v.nodes.map((n) => n.target.join(' ')).join(', ')}`);
    expect(found).toEqual([]);
  });
}

test('a backend that lists fine keeps its tick and no error line', async ({ page }) => {
  await page.route('**/health', (route) => route.fulfill({ json: {
    status: 'ready', backends: { ollama: true, openai: false, anthropic: true, gemini: false }, catalog_errors: {},
  } }));
  await page.goto(await server.dashboardUrl());
  await expect(badge(page, 'Backends')).toHaveClass(/badge-ok/);
  await expect(page.locator('#backends-status')).toContainText('Anthropic ✓');
  await expect(page.locator('#backends-errors')).toBeHidden();
});

test('the model list replaces what /health had cached: a fixed key clears the warning, a new failure shows', async ({ page }) => {
  await page.route('**/health', (route) => route.fulfill({ json: {
    status: 'ready', backends: { ollama: true, openai: false, anthropic: true, gemini: false },
    catalog_errors: { anthropic: 'anthropic: HTTP 401: invalid x-api-key' },
  } }));
  await page.route('**/models', (route) => route.fulfill({ json: {
    default: 'llama3.1:8b', models: [{ id: 'claude-fable-5-2', category: 'cloud', provider: 'anthropic', available: true }],
    errors: { ollama: 'ollama: Could not establish connection' },
  } }));
  await page.goto(await server.dashboardUrl());
  await expect(page.locator('#backends-status')).toContainText('Anthropic ⚠');
  await page.locator('#ask-model').focus();
  await expect(page.locator('#backends-status')).toContainText('Anthropic ✓');
  await expect(page.locator('#backends-status')).toContainText('Ollama ⚠');
  await expect(page.locator('#backends-errors')).toHaveText('Ollama: Could not establish connection');
  await expect(page.locator('#ask-model-errors')).toBeVisible();
  await expect(page.locator('#ask-model-errors')).toHaveText('Model list incomplete: Ollama: Could not establish connection');
});
