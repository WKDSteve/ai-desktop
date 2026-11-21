import { expect, test } from '@playwright/test';
import { startServer } from '../tests/helpers/server.mjs';

// The log's scrollbar is styled with the standard properties (scrollbar-width, scrollbar-color), not with
// ::-webkit-scrollbar, and takes its colours from the palette in both schemes.
let server;
test.beforeAll(async () => {
  server = await startServer();
});
test.afterAll(async () => {
  await server?.stop?.();
});

for (const colorScheme of ['light', 'dark']) {
  test(`the log scrollbar uses scrollbar-color from the palette (${colorScheme})`, async ({ page }) => {
    await page.emulateMedia({ colorScheme });
    await page.goto(await server.dashboardUrl());
    const style = await page.locator('#log').evaluate((el) => {
      const probe = document.createElement('i');
      probe.style.color = 'var(--line)';
      document.body.append(probe);
      const line = getComputedStyle(probe).color;
      probe.remove();
      const cs = getComputedStyle(el);
      return { width: cs.scrollbarWidth, color: cs.scrollbarColor, line };
    });
    expect(style.width).toBe('thin');
    expect(style.color).toContain(style.line); // the thumb is the palette's line colour
  });
}

test('no rule of the stylesheet uses ::-webkit-scrollbar', async ({ page }) => {
  await page.goto(await server.dashboardUrl());
  const css = await page.evaluate(() => fetch('/style.css').then((r) => r.text()));
  expect(css).not.toContain('::-webkit-scrollbar');
});
