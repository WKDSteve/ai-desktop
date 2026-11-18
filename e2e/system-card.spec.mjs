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
