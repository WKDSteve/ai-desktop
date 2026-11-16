import AxeBuilder from '@axe-core/playwright';
import { expect, test } from '@playwright/test';
import { startServer } from '../tests/helpers/server.mjs';

// The language of the dashboard: the first language of the browser that is German or English, English when it names
// neither (as the other apps of the family). Playwright's `locale` sets navigator.languages and Accept-Language.
let server;
test.beforeAll(async () => {
  server = await startServer();
});
test.afterAll(async () => {
  await server?.stop?.();
});

// Words of the German page that must not show on the English one.
const GERMAN = /Anfragen|Nutzung|Senden|Zeitraum|lädt|Verbinde|Agenten|Erlauben|Ereignisse|Aktualisieren/;

async function open(page) {
  await page.goto(`http://127.0.0.1:${server.port}/`);
  await expect(page.locator('#backends-status')).not.toHaveText(/lädt|loading/);
}

test.describe('English', () => {
  test.use({ locale: 'en-US' });

  test('the page speaks English, with no German word left', async ({ page }) => {
    await open(page);
    await expect(page.locator('html')).toHaveAttribute('lang', 'en');
    await expect(page).toHaveTitle('ai Dashboard');
    await expect(page.locator('#app-title')).toHaveText('▸ ai Dashboard');
    await expect(page.getByRole('button', { name: '▸ Send' })).toBeVisible();
    await expect(page.getByRole('combobox', { name: 'Period' })).toHaveValue('7d');
    await expect(page.getByPlaceholder('Enter a prompt...')).toBeVisible();
    await expect(page.locator('#usage-status')).toContainText('Metadata only');
    const text = await page.locator('body').innerText();
    expect(text).not.toMatch(GERMAN);
  });

  test('the English page has no axe violation', async ({ page }) => {
    await open(page);
    const result = await new AxeBuilder({ page })
      .withTags(['wcag2a', 'wcag2aa', 'wcag21a', 'wcag21aa', 'best-practice'])
      .analyze();
    expect(result.violations.map((v) => v.id)).toEqual([]);
  });
});

test.describe('German', () => {
  test.use({ locale: 'de-DE' });

  test('the page speaks German as before', async ({ page }) => {
    await open(page);
    await expect(page.locator('html')).toHaveAttribute('lang', 'de');
    await expect(page).toHaveTitle('KI Dashboard');
    await expect(page.getByRole('button', { name: '▸ Senden' })).toBeVisible();
    await expect(page.getByRole('combobox', { name: 'Zeitraum' })).toHaveValue('7d');
    await expect(page.locator('#usage-status')).toContainText('Nur Metadaten');
  });
});

test.describe('neither German nor English', () => {
  test.use({ locale: 'fr-FR' });

  test('falls back to English', async ({ page }) => {
    await open(page);
    await expect(page.locator('html')).toHaveAttribute('lang', 'en');
    await expect(page.getByRole('button', { name: '▸ Send' })).toBeVisible();
  });
});

test('the first of the languages that the page has wins', async ({ browser }) => {
  const context = await browser.newContext({ locale: 'fr-FR', extraHTTPHeaders: {} });
  const page = await context.newPage();
  // navigator.languages in the order French, German, English: German is the first the page has.
  await page.addInitScript(() => {
    Object.defineProperty(navigator, 'languages', { get: () => ['fr-FR', 'de-DE', 'en-US'] });
  });
  await open(page);
  await expect(page.locator('html')).toHaveAttribute('lang', 'de');
  await context.close();
});
