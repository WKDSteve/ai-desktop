import { expect, test } from '@playwright/test';
import { startServer } from '../tests/helpers/server.mjs';

// The x in the choice of the provider (the usage panel): it shows only while a provider is chosen, a click or Delete
// takes the choice away, the focus goes back to the choice, and the usage is asked again without a provider.
let server;
test.beforeAll(async () => {
  server = await startServer();
});
test.afterAll(async () => {
  await server?.stop?.();
});

test('the x in the choice of the provider takes the choice away in one click', async ({ page }) => {
  await page.goto(`http://127.0.0.1:${server.port}/`);
  await expect(page.locator('#backends-status')).not.toHaveText(/lädt/);
  const choice = page.getByRole('combobox', { name: 'Provider' });
  const clear = page.getByRole('button', { name: 'Auswahl Provider entfernen' });
  await expect(clear).toBeHidden();

  // A fresh server has no usage yet, so no provider to choose: one is put in, as the panel would after a call.
  // eslint-disable-next-line no-undef
  await page.evaluate(() => renderProviderSelect({ anthropic: {} }));
  const asked = [];
  page.on('request', (r) => {
    if (new URL(r.url()).pathname === '/usage') asked.push(new URL(r.url()).searchParams.get('provider'));
  });
  await choice.selectOption('anthropic');
  await expect(clear).toBeVisible();
  await expect.poll(() => asked.at(-1)).toBe('anthropic');

  await clear.click();
  await expect(choice).toHaveValue('');
  await expect(choice).toBeFocused();
  await expect(clear).toBeHidden();
  await expect.poll(() => asked.at(-1)).toBeNull();

  // The keyboard: Delete in the choice does the same.
  await choice.selectOption('anthropic');
  await choice.focus();
  await page.keyboard.press('Delete');
  await expect(choice).toHaveValue('');
  await expect(clear).toBeHidden();
});
