import { expect, test } from '@playwright/test';
import { startServer } from '../tests/helpers/server.mjs';

// an empty prompt says so at the field (aria-invalid, aria-describedby, focus); a job is sent once, its Run
// button is disabled from the click on.
test.use({ locale: 'en-US' });

let server;
test.beforeAll(async () => {
  server = await startServer();
});
test.afterAll(async () => {
  await server?.stop?.();
});

const JOB_ID = '0123456789abcdef';

for (const empty of ['', '   \n ']) {
  test(`an empty prompt (${JSON.stringify(empty)}) is named at the field and takes the focus`, async ({ page }) => {
    let asked = 0;
    await page.route(/\/ask\/stream$/, (route) => {
      asked += 1;
      return route.fulfill({ status: 500, body: '' });
    });
    await page.goto(await server.dashboardUrl());
    const field = page.locator('#ask-prompt');
    await field.fill(empty);
    await page.getByRole('button', { name: /Send/ }).click();

    await expect(field).toHaveAttribute('aria-invalid', 'true');
    await expect(field).toBeFocused();
    const describedBy = await field.getAttribute('aria-describedby');
    expect(describedBy).toBeTruthy();
    const message = page.locator(`#${describedBy}`);
    await expect(message).toBeVisible();
    await expect(message).toHaveText('Please enter a prompt.');
    expect(asked).toBe(0);

    // typing takes the error back
    await field.fill('hello');
    await expect(field).not.toHaveAttribute('aria-invalid', 'true');
    await expect(field).not.toHaveAttribute('aria-describedby', /./);
    await expect(message).toBeHidden();
  });
}

test('Run is disabled from the click on and the job is posted once', async ({ page }) => {
  const job = {
    id: JOB_ID, title: 'T', source: 'tasks-desktop', kind: 'plan', status: 'prefilled', task_id: 'x/y', area: 'x',
    markdown: 'm', model: '', output: '', runner: 'model', private: false,
  };
  let posts = 0;
  await page.route(/\/jobs$/, (route) => route.fulfill({ json: { jobs: [job] } }));
  await page.route(new RegExp(`/jobs/${JOB_ID}$`), (route) => route.fulfill({ json: job }));
  await page.route(new RegExp(`/jobs/${JOB_ID}/run$`), async (route) => {
    posts += 1;
    await new Promise((resolve) => setTimeout(resolve, 800));
    await route.fulfill({ status: 202, json: {} });
  });
  await page.route(/\/events$/, (route) => route.fulfill({ status: 200, contentType: 'text/event-stream', body: '' }));
  await page.goto((await server.dashboardUrl()).replace(/#.*/, '') + `#job=${JOB_ID}`);
  const run = page.locator('#job-run');
  await expect(run).toBeEnabled();

  await run.click();
  await expect(run).toBeDisabled(); // at once, while the request is on its way
  await run.dispatchEvent('click'); // a second click that still gets through
  await expect(run).toBeEnabled(); // the answer is in, the job is not running: Run again is possible
  expect(posts).toBe(1);
});
