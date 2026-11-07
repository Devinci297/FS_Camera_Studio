// XP Camera Studio: in X-Plane (?host=xplane) the Bindings tab lists each action's xpcs/ command, with Copy, instead of
// capturing keys, and the update and traffic texts name X-Plane. Run: node ui/tests/xplane-bindings.test.cjs
// (FSCS_PLAYWRIGHT may point at a Playwright install; FSCS_BROWSER at a Chromium or Edge executable.)
const {chromium} = require(process.env.FSCS_PLAYWRIGHT || 'playwright');
const assert = require('node:assert/strict'), path = require('node:path');

(async () => {
  const browser = await chromium.launch(process.env.FSCS_BROWSER ? {executablePath: process.env.FSCS_BROWSER} : {});
  try {
    const page = await browser.newPage({viewport: {width: 1100, height: 800}}), errors = [];
    page.on('pageerror', e => errors.push(e.message));
    await page.addInitScript(() => { window.xplane = {fscs: () => true, fscsPanel: () => true}; });
    const file = path.resolve(__dirname, '..', 'index.html').replaceAll('\\', '/');
    await page.goto('file:///' + file.replace(/^\//, '') + '?surface=main&host=xplane');
    await page.waitForFunction(() => window.FSCApp && window.FSCApp.bridge.mode === 'xplane');
    await page.locator('#mc-nav-bindings').click();

    assert.equal(await page.locator('#mc-binding-status').isVisible(), false, 'no assignment filter: assignments live in X-Plane');
    assert.equal(await page.locator('#mc-binding-kind').isVisible(), false, 'no input filter');
    assert.match(await page.locator('#mc-page-bindings .mc-page-header p').innerText(), /X-Plane command/);
    assert.match(await page.locator('#mc-binding-result').innerText(), /Keyboard or Joystick settings/);
    assert.equal(await page.locator('[data-binding]').count(), 0, 'no MSFS capture rows');

    const commands = await page.locator('.mc-binding-command').evaluateAll(els => els.map(e => e.value));
    for (const c of ['xpcs/next_camera', 'xpcs/toggle_editor', 'xpcs/toggle_mini', 'xpcs/panic_release', 'xpcs/glance_hold',
      'xpcs/spot_next', 'xpcs/toggle_auto_spot', 'xpcs/move_fast', 'xpcs/binoculars', 'xpcs/look_left']) {
      assert.ok(commands.includes(c), c + ' listed');
    }
    assert.ok(commands.every(c => /^xpcs\/[a-z0-9_]+$/.test(c)), 'command names are xpcs/snake_case');
    assert.equal(await page.locator('.mc-binding-command').first().getAttribute('readonly'), '');

    await page.fill('#mc-binding-search', 'spot_');
    assert.equal(await page.locator('#mc-binding-count').innerText(), '3 of 35 bindings', 'search matches the command text');
    await page.fill('#mc-binding-search', 'cinematic');
    assert.deepEqual(await page.locator('.mc-binding-command').evaluateAll(els => els.map(e => e.value)), ['xpcs/toggle_cinematic']);
    await page.locator('.mc-binding-xp button', {hasText: 'Copy'}).click();
    assert.match(await page.locator('#mc-binding-result').innerText(), /xpcs\/toggle_cinematic/);

    assert.match(await page.locator('#mc-update-settings').innerText(), /close X-Plane and run setup/);
    assert.deepEqual(errors, []);
    console.log('X-Plane bindings: xpcs/ commands with Copy, command search, hidden MSFS filters and X-Plane update wording passed.');
  } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
