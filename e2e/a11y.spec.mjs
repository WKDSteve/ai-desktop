import AxeBuilder from '@axe-core/playwright';
import { expect, test } from '@playwright/test';
import { startServer } from '../tests/helpers/server.mjs';

// Accessibility of the dashboard (axe), light and dark, against the real binary (build it first, or set
// AI_DESKTOP_BIN): the page as it loads, with the decision banner shown, and with the pointer on and the focus in the
// buttons, since a hover or a focus colour of the palette must keep its contrast too.
const TAGS = ['wcag2a', 'wcag2aa', 'wcag21a', 'wcag21aa', 'best-practice'];

let server;
test.beforeAll(async () => {
  server = await startServer();
});
test.afterAll(async () => {
  await server?.stop?.();
});

async function open(page) {
  await page.goto(`http://127.0.0.1:${server.port}/`);
  // The page has asked the server what it has: the backends and the usage are filled in.
  await expect(page.locator('#backends-status')).not.toHaveText(/lädt/);
}

async function violations(page) {
  const result = await new AxeBuilder({ page }).withTags(TAGS).analyze();
  return result.violations.map((v) => [v.id, v.nodes.map((n) => n.html)]);
}

for (const scheme of ['light', 'dark']) {
  test.describe(`axe, ${scheme}`, () => {
    test.beforeEach(async ({ page }) => {
      await page.emulateMedia({ colorScheme: scheme, reducedMotion: 'reduce' });
    });

    test('the page follows the scheme and has no violation', async ({ page }) => {
      await open(page);
      const bg = await page.evaluate(() => getComputedStyle(document.body).backgroundColor);
      const [r] = bg.match(/\d+/g).map(Number);
      expect(scheme === 'light' ? r > 200 : r < 60, `body ${bg} in ${scheme}`).toBe(true);
      expect(await violations(page)).toEqual([]);
    });

    test('with the decision banner shown', async ({ page }) => {
      await open(page);
      await page.evaluate(() => {
        document.getElementById('decision-text').textContent = 'rm -rf build/ ausführen?';
        document.getElementById('decision-banner').classList.add('visible');
      });
      await expect(page.getByRole('button', { name: /Erlauben/ })).toBeVisible();
      expect(await violations(page)).toEqual([]);
      await page.getByRole('button', { name: /Ablehnen/ }).hover();
      expect(await violations(page)).toEqual([]);
    });

    test('with the pointer on the main button and the focus on another', async ({ page }) => {
      await open(page);
      await page.getByRole('button', { name: /Senden/ }).hover();
      expect(await violations(page)).toEqual([]);
      await page.getByRole('button', { name: /Aktualisieren/ }).focus();
      await page.keyboard.press('Shift+Tab');
      await page.keyboard.press('Tab');
      expect(await violations(page)).toEqual([]);
    });
  });
}
