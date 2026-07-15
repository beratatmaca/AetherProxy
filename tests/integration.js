/**
 * AetherProxy Playwright integration tests.
 *
 * Run with:
 *   node tests/integration.js
 *
 * Requires the binary at ./build/aetherproxy and Playwright installed
 * (npm install in tests/).
 */

'use strict';

const { chromium, firefox } = require('@playwright/test');
const { spawn } = require('child_process');
const assert = require('assert');
const http = require('http');

// ── helpers ──────────────────────────────────────────────────────────────────

function fetchText(url) {
    return new Promise((resolve, reject) => {
        http.get(url, res => {
            let body = '';
            res.on('data', d => (body += d));
            res.on('end', () => resolve({ status: res.statusCode, body }));
        }).on('error', reject);
    });
}

function wait(ms) {
    return new Promise(r => setTimeout(r, ms));
}

const PORT = 8095;
const ROOM_CODE_RE = /^[a-z]+-[a-z]+-[a-z]+-\d{5}$/;

// ── test runner ───────────────────────────────────────────────────────────────

async function runTests(browserType, browserName) {
    console.log(`\n=== Running tests in ${browserName} ===`);

    // 1. Launch host binary and wait for it to be ready.
    const host = spawn('./build/aetherproxy', ['--port', String(PORT)]);
    let roomCode = '';

    const ready = new Promise((resolve, reject) => {
        const timer = setTimeout(() => reject(new Error('Host startup timeout')), 10_000);
        host.stdout.on('data', data => {
            const text = data.toString();
            const m = text.match(/Room:\s*([a-z]+-[a-z]+-[a-z]+-\d{5})/i);
            if (m) roomCode = m[1].trim();
            if (text.includes('Server listening on port')) {
                clearTimeout(timer);
                resolve();
            }
        });
        host.stderr.on('data', d => process.stderr.write('[host] ' + d));
    });

    try {
        await ready;
    } catch (e) {
        host.kill();
        throw e;
    }

    // 2. Room code format.
    console.log('  [1] Room code format:', roomCode);
    assert.match(roomCode, ROOM_CODE_RE, `Room code "${roomCode}" does not match word-word-word-NNNNN`);

    // 3. HTTP server responds on expected port.
    const indexRes = await fetchText(`http://localhost:${PORT}/`);
    assert.strictEqual(indexRes.status, 200, 'GET / should return 200');
    assert.ok(indexRes.body.includes('AetherProxy'), 'Index page should mention AetherProxy');
    console.log('  [2] HTTP server OK');

    // 4. SDP offer is served and looks like valid SDP.
    const offerRes = await fetchText(`http://localhost:${PORT}/offer?id=test-client-001`);
    assert.strictEqual(offerRes.status, 200, 'GET /offer should return 200');
    assert.ok(offerRes.body.startsWith('v=0'), `SDP offer should start with "v=0", got: ${offerRes.body.slice(0, 30)}`);
    assert.ok(offerRes.body.includes('m=application'), 'SDP offer should contain datachannel media line');
    console.log('  [3] SDP offer served OK');

    // 5. Browser DOM assertions.
    const browser = await browserType.launch({ headless: true });
    const page = await browser.newPage();

    try {
        await page.goto(`http://localhost:${PORT}/`);

        // Give the page a moment to load scripts (no WebRTC needed for DOM checks).
        await wait(1500);

        // 5a. Page title.
        const title = await page.title();
        assert.ok(title.includes('AetherProxy'), `Title should include AetherProxy, got: "${title}"`);
        console.log('  [4] Page title OK');

        // 5b. xterm.js canvas mounts.
        const canvas = await page.$('.xterm-screen canvas');
        assert.ok(canvas !== null, 'xterm.js canvas should be in the DOM');
        console.log('  [5] xterm canvas mounted');

        // 5c. Toolbar buttons are present.
        for (const id of ['btn-ctrl', 'btn-alt', 'btn-tab', 'btn-esc',
                           'btn-up', 'btn-down', 'btn-left', 'btn-right']) {
            const el = await page.$(`#${id}`);
            assert.ok(el !== null, `Toolbar button #${id} should be in the DOM`);
        }
        console.log('  [6] Toolbar buttons present');

        // 5d. Mode badge element exists (hidden by default in terminal mode).
        const badge = await page.$('#mode-badge');
        assert.ok(badge !== null, '#mode-badge element should exist in DOM');
        const badgeDisplay = await badge.evaluate(el => getComputedStyle(el).display);
        assert.strictEqual(badgeDisplay, 'none', 'Mode badge should be hidden in terminal mode');
        console.log('  [7] Mode badge hidden in terminal mode');

        // 5e. Ctrl/Alt buttons toggle modifier-active class (not no-ops).
        await page.click('#btn-ctrl');
        const ctrlActive = await page.$('#btn-ctrl.modifier-active');
        assert.ok(ctrlActive !== null, '#btn-ctrl should gain modifier-active class on click');
        await page.click('#btn-ctrl'); // toggle off
        const ctrlOff = await page.$('#btn-ctrl.modifier-active');
        assert.strictEqual(ctrlOff, null, '#btn-ctrl should lose modifier-active class on second click');
        console.log('  [8] Ctrl modifier toggle OK');

        // 5f. Keyboard is NOT disabled in terminal mode (isPipeMode = false by default).
        //     We verify by checking that term.onData is registered: proxy via window.isPipeMode.
        const pipeMode = await page.evaluate(() => window.isPipeMode ?? false);
        // isPipeMode is a local variable, not window-exposed — verify indirectly:
        // In terminal mode the terminal container should not have a pointer-events:none style.
        const termStyle = await page.$eval('#terminal-container',
            el => getComputedStyle(el).pointerEvents);
        assert.notStrictEqual(termStyle, 'none', 'Terminal should be interactive in terminal mode');
        console.log('  [9] Keyboard not locked in terminal mode');

    } finally {
        await browser.close();
    }

    host.kill();
    await new Promise(r => host.on('exit', r));
    console.log(`  ✓ All ${browserName} tests passed`);
}

// ── main ──────────────────────────────────────────────────────────────────────

(async () => {
    let failed = false;
    for (const [type, name] of [[chromium, 'Chromium'], [firefox, 'Firefox']]) {
        try {
            await runTests(type, name);
        } catch (e) {
            console.error(`\n✗ ${name} test FAILED:`, e.message);
            failed = true;
        }
    }
    process.exit(failed ? 1 : 0);
})();
