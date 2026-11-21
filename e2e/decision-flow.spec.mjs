import { expect, test } from '@playwright/test';
import { startServer } from '../tests/helpers/server.mjs';

// The live path of the dashboard, with nothing faked in the page: the SSE connection to the running server, the
// decision point of the simulation agent (about 8 s after the server started, so no option shortens it: the test
// simply waits for it), and the answer going back through POST /decision. Replaces the manual run of section 8 of
// the ai live-testing plan. One server per test: the agent's events are read once, by the first page that connects.
test.use({ locale: 'en-US' });

let server;
test.beforeEach(async () => {
  server = await startServer();
});
test.afterEach(async () => {
  await server?.stop?.();
});

test('the connection indicator turns to connected', async ({ page }) => {
  await page.goto(await server.dashboardUrl());
  await expect(page.locator('#sse-label')).toHaveText('Connected');
  await expect(page.locator('#sse-status')).toHaveText('Connected');
  await expect(page.locator('.badge', { hasText: 'SSE' })).toHaveClass(/badge-ok/);
});

for (const [button, answer, sent] of [
  ['Allow', 'allow', 'Decision sent: allow'],
  ['Deny', 'deny', 'Decision sent: deny'],
]) {
  test(`${answer}: the real banner goes away, the log shows the answer and the server gets it`, async ({ page }) => {
    test.setTimeout(60_000);
    await page.goto(await server.dashboardUrl());
    const banner = page.locator('#decision-banner');
    await expect(banner).toBeVisible({ timeout: 30_000 });
    await expect(page.locator('#decision-text')).toHaveText('Shall I start the first agent container now?');
    // The agent that asked waits.
    await expect(page.locator('#dot-Planner')).toHaveClass(/waiting/);

    await banner.getByRole('button', { name: new RegExp(button) }).click();

    await expect(banner).toBeHidden();
    await expect(page.locator('#log')).toContainText(sent);
    // The server saw the answer: it prints it as an event of "human" (and sends it back on the stream).
    await expect.poll(() => server.output(), { timeout: 10_000 }).toMatch(new RegExp(String.raw`\[human\] Decision \[\d+\] -> ${answer}`));
    await expect(page.locator('#log')).toContainText(new RegExp(String.raw`Decision \[\d+\] -> ${answer}`));
  });
}
