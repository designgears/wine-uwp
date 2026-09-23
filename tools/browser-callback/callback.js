// Open URL in a visible Chrome with a throwaway profile, and print the first navigation to
// SCHEME: (e.g. shapr3d:) - the redirect an app's web sign-in ends with. The human does the
// sign-in, CAPTCHA included; this only watches for the hand-back. Usage: node callback.js URL SCHEME
const { chromium } = require('playwright-core');
const fs = require('fs');
const [url, scheme] = process.argv.slice(2);
const chrome = ['/usr/bin/google-chrome-stable', '/usr/bin/google-chrome', '/usr/bin/chromium',
                '/usr/bin/microsoft-edge-stable'].find(p => fs.existsSync(p));
(async () => {
  const browser = await chromium.launch({ executablePath: chrome, headless: false });
  const page = await browser.newPage();
  const done = new Promise(resolve => {
    const check = u => { if (u.toLowerCase().startsWith(scheme.toLowerCase() + ':')) resolve(u); };
    page.context().on('request', r => check(r.url()));
    page.context().on('page', p => p.on('request', r => check(r.url())));
  });
  await page.goto(url);
  const callback = await done;
  console.log(callback);
  await browser.close();
})().catch(e => { console.error(e.message); process.exit(1); });
