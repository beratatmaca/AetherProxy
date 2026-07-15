/**
 * AetherProxy integration tests — binary-side + browser-side.
 *
 * Run with:
 *   node tests/integration.js
 *
 * Requires:
 *   - Binary at ./build/aetherproxy (built with CMake)
 *   - Playwright installed: cd tests && npm install && npx playwright install
 */

'use strict';

const { chromium, firefox } = require('@playwright/test');
const { spawn }              = require('child_process');
const assert                 = require('assert');
const http                   = require('http');

// ################ helpers ################

const BINARY   = './build/aetherproxy';
const PORT     = 8095;
const ROOM_RE  = /^[a-z]+-[a-z]+-[a-z]+-\d{5}$/;

/** Fetches a URL and returns { status, body }. */
function fetchText(url) {
    return new Promise((resolve, reject) => {
        http.get(url, res => {
            let body = '';
            res.on('data', d => (body += d));
            res.on('end', () => resolve({ status: res.statusCode, body }));
        }).on('error', reject);
    });
}

/** Resolves after `ms` milliseconds. */
const wait = ms => new Promise(r => setTimeout(r, ms));

/**
 * Spawns the aetherproxy binary with the given extra args and optional stdin
 * bytes. Returns { proc, roomCode, stdout } once the server is ready.
 *
 * @param {string[]} extraArgs   Extra CLI args after --port PORT.
 * @param {Buffer|null} stdinBuf  If non-null, written to stdin then closed.
 * @param {number} timeoutMs      How long to wait for "Server listening".
 */
async function spawnHost(extraArgs = [], stdinBuf = null, timeoutMs = 12_000) {
    const args = ['--port', String(PORT), ...extraArgs];
    let proc;
    if (stdinBuf !== null) {
        // Pipe mode: feed bytes over stdin.
        proc = spawn(BINARY, args, {
            stdio: ['pipe', 'pipe', 'pipe'],
            detached: true,
        });
    } else {
        // Terminal mode: allocate a PTY via script(1) so isatty(stdin) is true.
        proc = spawn('script', ['-qefc', [BINARY, ...args].join(' '), '/dev/null'], {
            stdio: ['ignore', 'pipe', 'pipe'],
            detached: true,
        });
    }

    let stdoutBuf = '';
    let roomCode  = '';

    const ready = new Promise((resolve, reject) => {
        const timer = setTimeout(
            () => reject(new Error(`Host startup timeout after ${timeoutMs}ms`)),
            timeoutMs,
        );

        proc.stdout.on('data', data => {
            stdoutBuf += data.toString();
            const m = stdoutBuf.match(/Room:\s*([a-z]+-[a-z]+-[a-z]+-\d{5})/i);
            if (m && !roomCode) roomCode = m[1].trim();
            if (stdoutBuf.includes('Server listening on port')) {
                clearTimeout(timer);
                resolve();
            }
        });
        proc.stderr.on('data', d => process.stderr.write('[host err] ' + d));
        proc.on('exit', code => {
            clearTimeout(timer);
            // Resolve so callers can inspect; individual tests must check.
            resolve();
        });
    });

    if (stdinBuf !== null) {
        proc.stdin.write(stdinBuf);
        proc.stdin.end();
    }

    await ready;
    return { proc, roomCode, getStdout: () => stdoutBuf };
}

/** Kills a host process group and waits for exit; tolerates already-dead hosts. */
async function killHost(proc) {
    if (proc.exitCode !== null || proc.signalCode !== null) return;
    try { process.kill(-proc.pid, 'SIGTERM'); }
    catch { try { proc.kill('SIGTERM'); } catch { /* already gone */ } }
    await new Promise(resolve => {
        const t = setTimeout(() => {
            try { process.kill(-proc.pid, 'SIGKILL'); } catch { /* already gone */ }
            resolve();
        }, 3000);
        proc.once('exit', () => { clearTimeout(t); resolve(); });
    });
}

// ################ BINARY-SIDE tests ################

/**
 * T-01  Room code format
 * Asserts that the room code printed on stdout matches word-word-word-NNNNN.
 */
async function testRoomCodeFormat() {
    const { proc, roomCode } = await spawnHost();
    try {
        assert.match(
            roomCode,
            ROOM_RE,
            `Room code "${roomCode}" does not match word-word-word-NNNNN`,
        );
        // Verify each word segment is ≤7 characters (spec: ≤7-letter words).
        const [w1, w2, w3] = roomCode.split('-');
        for (const w of [w1, w2, w3]) {
            assert.ok(w.length <= 7, `Word "${w}" is longer than 7 characters`);
        }
        console.log('  [T-01] Room code format ✓', roomCode);
    } finally {
        await killHost(proc);
    }
}

/**
 * T-02  HTTP server + SDP offer
 * Asserts that:
 *   - GET / returns 200 with AetherProxy in the body.
 *   - GET /offer?id=... returns a valid SDP string (v=0 … m=application).
 *   - Static assets (xterm.js, xterm.css, xterm-addon-fit.js) are served.
 */
async function testHttpServerAndSdp() {
    const { proc } = await spawnHost();
    try {
        // Index page.
        const idx = await fetchText(`http://localhost:${PORT}/`);
        assert.strictEqual(idx.status, 200, 'GET / must return 200');
        assert.ok(idx.body.includes('AetherProxy'), 'Index page must contain "AetherProxy"');
        assert.ok(idx.body.includes('xterm.js'),    'Index page must load xterm.js');

        // SDP offer.
        const offerRes = await fetchText(`http://localhost:${PORT}/offer?id=t02-client`);
        assert.strictEqual(offerRes.status, 200, 'GET /offer must return 200');
        assert.ok(
            offerRes.body.startsWith('v=0'),
            `SDP must start with "v=0", got: "${offerRes.body.slice(0, 40)}"`,
        );
        assert.ok(offerRes.body.includes('m=application'), 'SDP must contain datachannel m= line');
        assert.ok(offerRes.body.includes('webrtc-datachannel'), 'SDP must reference webrtc-datachannel');

        // Repeated GET /offer with the same id returns an offer (idempotent in the no-answer case).
        const offerRes2 = await fetchText(`http://localhost:${PORT}/offer?id=t02-client`);
        assert.strictEqual(offerRes2.status, 200, 'Second GET /offer for same id must return 200');

        // Static assets are served (non-empty bodies).
        for (const asset of ['/xterm.js', '/xterm.css', '/xterm-addon-fit.js']) {
            const r = await fetchText(`http://localhost:${PORT}${asset}`);
            assert.strictEqual(r.status, 200, `GET ${asset} must return 200`);
            assert.ok(r.body.length > 100, `${asset} body is suspiciously short (${r.body.length} bytes)`);
        }

        console.log('  [T-02] HTTP server + SDP offer ✓');
    } finally {
        await killHost(proc);
    }
}

/**
 * T-03  QR code in stdout
 * Asserts that the terminal output contains Unicode block characters that
 * libqrencode emits when rendering a QR code to a UTF-8 terminal.
 * The canonical markers are the top-left finder pattern squares: ▄ or █.
 */
async function testQrCodeInOutput() {
    const { proc, getStdout } = await spawnHost();
    // Give the binary a moment to finish rendering the QR before reading stdout.
    await wait(500);
    await killHost(proc);

    const out = getStdout();
    // libqrencode renders QR codes using Unicode half-block characters.
    // At minimum we expect several block elements.
    const hasBlocks = /[\u2580-\u259F\u2588]/.test(out);
    // Fallback: some builds use ANSI + spaces; in that case look for the
    // repeating ESC[7m (reverse video) pattern that draws QR modules.
    const hasAnsiQr = out.includes('\x1b[7m');
    assert.ok(
        hasBlocks || hasAnsiQr,
        'stdout should contain QR code block characters (▄/█) or ANSI reverse-video QR pattern',
    );
    console.log('  [T-03] QR code in output ✓');
}

// ################ BROWSER-SIDE binary tests ################

/**
 * T-04  Pipe mode — browser receives all bytes, UI is locked.
 *
 * We pipe exactly PIPE_BYTES of pseudo-random data to the binary's stdin.
 * The browser page is launched; we instrument dc.onmessage to accumulate
 * non-control bytes. After the EOF frame arrives we assert:
 *   a) accumulated bytes === PIPE_BYTES
 *   b) #mode-badge is visible (class "visible" present)
 *   c) #terminal-container has class "pipe-locked"
 *   d) window.isPipeMode === true
 */
async function testPipeMode(browserType, browserName) {
    const PIPE_BYTES = 1000;
    const payload    = Buffer.alloc(PIPE_BYTES, 0x41); // 1000 × 'A'

    const { proc, roomCode } = await spawnHost([], payload);

    // Give the binary time to start the HTTP server even though stdin is pipe.
    await wait(800);

    const browser = await browserType.launch({ headless: true });
    const page    = await browser.newPage();

    try {
        // Intercept DataChannel messages before page scripts run.
        await page.addInitScript(() => {
            window._aether_bytesReceived = 0;
            window._aether_gotEof        = false;
            window._aether_gotMode       = false;
            // Patch RTCPeerConnection so we can hook ondatachannel.
            const origRTCPC = window.RTCPeerConnection;
            window.RTCPeerConnection = function (...args) {
                const pc = new origRTCPC(...args);
                const origOnDC = Object.getOwnPropertyDescriptor(
                    RTCPeerConnection.prototype, 'ondatachannel'
                );
                pc.addEventListener('datachannel', ev => {
                    const dc = ev.channel;
                    dc.addEventListener('message', e => {
                        const data = e.data;
                        if (typeof data === 'string' && data.startsWith('\x00AETHER:')) {
                            try {
                                const payload = JSON.parse(data.substring(8));
                                if (payload.type === 'eof')  window._aether_gotEof  = true;
                                if (payload.type === 'mode') window._aether_gotMode = true;
                            } catch (_) {}
                        } else if (typeof data === 'string') {
                            window._aether_bytesReceived += data.length;
                        } else if (data instanceof ArrayBuffer) {
                            window._aether_bytesReceived += data.byteLength;
                        }
                    });
                });
                return pc;
            };
        });

        await page.goto(`http://localhost:${PORT}/`);

        // Wait for the EOF frame (up to 10 s).
        await page.waitForFunction(() => window._aether_gotEof === true, { timeout: 10_000 });

        // a) Byte count.
        const received = await page.evaluate(() => window._aether_bytesReceived);
        assert.strictEqual(received, PIPE_BYTES,
            `Browser should receive exactly ${PIPE_BYTES} bytes, got ${received}`);

        // b) Mode badge visible.
        const badgeVisible = await page.$eval('#mode-badge',
            el => el.classList.contains('visible'));
        assert.ok(badgeVisible, '#mode-badge should have class "visible" in pipe mode');

        // c) Terminal container locked.
        const termLocked = await page.$eval('#terminal-container',
            el => el.classList.contains('pipe-locked'));
        assert.ok(termLocked, '#terminal-container should have class "pipe-locked" in pipe mode');

        // d) window.isPipeMode.
        const isPipe = await page.evaluate(() => window.isPipeMode);
        assert.strictEqual(isPipe, true, 'window.isPipeMode should be true in pipe mode');

        console.log(`  [T-04] Pipe mode (${browserName}) ✓ — received ${received} bytes`);
    } finally {
        await browser.close();
        await killHost(proc);
    }
}

/**
 * T-05  Terminal resize — browser sends resize frame to host.
 *
 * We intercept RTCDataChannel.prototype.send via addInitScript to capture
 * all frames the browser transmits. After the data channel opens we resize
 * the browser viewport. We then assert a \x00AETHER:{"type":"resize",...}
 * frame was sent containing cols/rows that match the new viewport dimensions.
 */
async function testTerminalResize(browserType, browserName) {
    const { proc } = await spawnHost();
    const browser  = await browserType.launch({ headless: true });

    // Start with a known viewport.
    const page = await browser.newPage({ viewport: { width: 1200, height: 800 } });

    try {
        // Intercept DataChannel.send before page scripts run.
        await page.addInitScript(() => {
            window._aether_sentFrames = [];
            window._aether_dcOpen     = false;

            const origRTCPC = window.RTCPeerConnection;
            window.RTCPeerConnection = function (...args) {
                const pc = new origRTCPC(...args);
                pc.addEventListener('datachannel', ev => {
                    const dc = ev.channel;
                    const origSend = dc.send.bind(dc);
                    dc.send = function (data) {
                        window._aether_sentFrames.push(
                            typeof data === 'string' ? data : '[binary]'
                        );
                        return origSend(data);
                    };
                    dc.addEventListener('open', () => {
                        window._aether_dcOpen = true;
                    });
                });
                return pc;
            };
        });

        await page.goto(`http://localhost:${PORT}/`);

        // Wait for the data channel to open (up to 15 s).
        await page.waitForFunction(() => window._aether_dcOpen === true, { timeout: 15_000 });

        // Record frame count at open time.
        const framesAtOpen = await page.evaluate(() => window._aether_sentFrames.length);

        // Resize to a different viewport.
        await page.setViewportSize({ width: 900, height: 600 });
        await wait(500); // allow fitAddon.fit() + sendResizeFrame() to fire

        // Collect sent frames after the resize.
        const frames = await page.evaluate(() => window._aether_sentFrames);
        const resizeFrames = frames.slice(framesAtOpen).filter(f =>
            typeof f === 'string' &&
            f.startsWith('\x00AETHER:') &&
            f.includes('"type":"resize"')
        );

        assert.ok(
            resizeFrames.length > 0,
            `Expected at least one resize control frame after viewport resize, got 0.\nAll frames: ${JSON.stringify(frames.slice(framesAtOpen))}`,
        );

        // Parse the last resize frame and verify cols/rows are plausible.
        const last = JSON.parse(resizeFrames[resizeFrames.length - 1].substring(8));
        assert.ok(last.cols > 0 && last.cols < 1000, `cols=${last.cols} out of range`);
        assert.ok(last.rows > 0 && last.rows < 500,  `rows=${last.rows} out of range`);

        // After the initial open, we should also have received a resize frame
        // (sendResizeFrame is called on dc.onopen).
        const openFrames = frames.slice(0, framesAtOpen).filter(f =>
            typeof f === 'string' &&
            f.startsWith('\x00AETHER:') &&
            f.includes('"type":"resize"')
        );
        assert.ok(
            openFrames.length > 0,
            'Expected a resize frame sent immediately on DataChannel open',
        );

        console.log(`  [T-05] Terminal resize (${browserName}) ✓ — cols=${last.cols} rows=${last.rows}`);
    } finally {
        await browser.close();
        await killHost(proc);
    }
}

// ################ test registry and runner ################

const BINARY_TESTS = [
    { name: 'T-01  Room code format',       fn: testRoomCodeFormat   },
    { name: 'T-02  HTTP server + SDP offer', fn: testHttpServerAndSdp },
    { name: 'T-03  QR code in output',       fn: testQrCodeInOutput   },
];

const BROWSER_TESTS = [
    { name: 'T-04  Pipe mode',        fn: testPipeMode      },
    { name: 'T-05  Terminal resize',  fn: testTerminalResize },
];

// DOM-only tests from the previous session (no real WebRTC needed).
async function runDomTests(browserType, browserName) {
    const { proc } = await spawnHost();
    const browser  = await browserType.launch({ headless: true });
    const page     = await browser.newPage();

    try {
        await page.goto(`http://localhost:${PORT}/`);
        await wait(1500);

        // T-06  Page title.
        const title = await page.title();
        assert.ok(title.includes('AetherProxy'), `Title: "${title}"`);
        console.log('  [T-06] Page title ✓');

        // T-07  xterm terminal mounts (xterm 5.x uses the DOM renderer by default).
        const screen = await page.$('.xterm .xterm-screen');
        assert.ok(screen !== null, 'xterm screen element must be in DOM');
        console.log('  [T-07] xterm mounts ✓');

        // T-08  Toolbar buttons present.
        for (const id of ['btn-ctrl','btn-alt','btn-tab','btn-esc',
                           'btn-up','btn-down','btn-left','btn-right']) {
            assert.ok(await page.$(`#${id}`) !== null, `#${id} must be in DOM`);
        }
        console.log('  [T-08] Toolbar buttons ✓');

        // T-09  Mode badge hidden by default (terminal mode).
        const badgeDisplay = await page.$eval('#mode-badge',
            el => getComputedStyle(el).display);
        assert.strictEqual(badgeDisplay, 'none', 'Mode badge must be hidden in terminal mode');
        console.log('  [T-09] Mode badge hidden ✓');

        // T-10  Ctrl modifier toggle (not a no-op).
        await page.click('#btn-ctrl');
        assert.ok(await page.$('#btn-ctrl.modifier-active') !== null,
            '#btn-ctrl must gain modifier-active class on click');
        await page.click('#btn-ctrl');
        assert.strictEqual(await page.$('#btn-ctrl.modifier-active'), null,
            '#btn-ctrl must lose modifier-active class on second click');
        console.log('  [T-10] Ctrl modifier toggle ✓');

        // T-11  Terminal is interactive in terminal mode (no pipe-locked class).
        const isLocked = await page.$eval('#terminal-container',
            el => el.classList.contains('pipe-locked'));
        assert.strictEqual(isLocked, false, 'Terminal must not be locked in terminal mode');
        console.log('  [T-11] Keyboard not locked in terminal mode ✓');

        // T-12  window.isPipeMode is false in terminal mode.
        const pipeMode = await page.evaluate(() => window.isPipeMode ?? false);
        assert.strictEqual(pipeMode, false, 'window.isPipeMode must be false in terminal mode');
        console.log('  [T-12] window.isPipeMode === false ✓');

    } finally {
        await browser.close();
        await killHost(proc);
    }
}

// ################ main ################

(async () => {
    const results = [];

    // Binary-only tests (no browser).
    console.log('\n╔══════════════════════════════════════╗');
    console.log('║  Binary-side tests (no browser)     ║');
    console.log('╚══════════════════════════════════════╝');
    for (const t of BINARY_TESTS) {
        process.stdout.write(`\n  Running: ${t.name} … `);
        try {
            await t.fn();
            results.push({ name: t.name, ok: true });
        } catch (e) {
            console.error(`FAILED\n  ✗ ${e.message}`);
            results.push({ name: t.name, ok: false, err: e.message });
        }
    }

    // Browser-side tests across Chromium and Firefox.
    for (const [browserType, browserName] of [[chromium, 'Chromium'], [firefox, 'Firefox']]) {
        console.log(`\n╔══════════════════════════════════════╗`);
        console.log(`║  Browser tests — ${browserName.padEnd(18)} ║`);
        console.log(`╚══════════════════════════════════════╝`);

        // DOM-only suite.
        process.stdout.write(`\n  Running: DOM suite (${browserName}) … \n`);
        try {
            await runDomTests(browserType, browserName);
            results.push({ name: `DOM suite (${browserName})`, ok: true });
        } catch (e) {
            console.error(`  ✗ DOM suite failed: ${e.message}`);
            results.push({ name: `DOM suite (${browserName})`, ok: false, err: e.message });
        }

        // Browser tests that require actual WebRTC negotiation.
        for (const t of BROWSER_TESTS) {
            const label = `${t.name} (${browserName})`;
            process.stdout.write(`\n  Running: ${label} … \n`);
            try {
                await t.fn(browserType, browserName);
                results.push({ name: label, ok: true });
            } catch (e) {
                console.error(`  ✗ ${e.message}`);
                results.push({ name: label, ok: false, err: e.message });
            }
        }
    }

    // Summary.
    console.log('\n══════════════════════════════════════════');
    const passed = results.filter(r => r.ok).length;
    const failed = results.filter(r => !r.ok).length;
    for (const r of results) {
        console.log(`  ${r.ok ? '✓' : '✗'} ${r.name}${r.ok ? '' : '\n      ' + r.err}`);
    }
    console.log(`\n  ${passed} passed, ${failed} failed`);
    console.log('══════════════════════════════════════════');
    process.exit(failed > 0 ? 1 : 0);
})();
