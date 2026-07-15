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

const { chromium, firefox }  = require('@playwright/test');
const { spawn, execFileSync } = require('child_process');
const assert                 = require('assert');
const http                   = require('http');
const net                    = require('net');
const fs                     = require('fs');
const os                     = require('os');
const path                   = require('path');

// ################ helpers ################

const BINARY   = process.env.BINARY || './build/aetherproxy';
const PORT     = Number(process.env.PORT) || 8095;
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

/** Waits until nothing accepts TCP connections on the port. */
function waitForPortFree(port, timeoutMs = 5000) {
    return new Promise(resolve => {
        const deadline = Date.now() + timeoutMs;
        const probe = () => {
            const sock = net.connect({ port, host: '127.0.0.1' });
            sock.once('connect', () => {
                sock.destroy();
                if (Date.now() > deadline) return resolve();
                setTimeout(probe, 100);
            });
            sock.once('error', () => resolve());
        };
        probe();
    });
}

/**
 * Returns an env with an isolated HOME whose saved config sets the test
 * port. The port flag is config-only now, so hosts pick it up from there.
 */
let TEST_HOME = null;
function testEnv() {
    if (!TEST_HOME) {
        TEST_HOME = fs.mkdtempSync(path.join(os.tmpdir(), 'aether-test-home-'));
        const env = { ...process.env, HOME: TEST_HOME };
        execFileSync(BINARY, ['config', 'set', 'port', String(PORT)], { env });
        // Offline keeps test hosts away from real STUN/TURN/signaling servers.
        execFileSync(BINARY, ['config', 'set', 'offline', 'true'], { env });
        // Admission prompts are covered by T-22; other tests auto-admit.
        execFileSync(BINARY, ['config', 'set', 'admit', 'false'], { env });
    }
    return { ...process.env, HOME: TEST_HOME };
}

/**
 * Spawns the aetherproxy binary with the given extra args and optional stdin
 * bytes. Returns { proc, roomCode, stdout } once the server is ready.
 *
 * @param {string[]} extraArgs   Extra CLI args.
 * @param {Buffer|null} stdinBuf  If non-null, written to stdin then closed.
 * @param {number} timeoutMs      How long to wait for "Server listening".
 */
async function spawnHost(extraArgs = [], stdinBuf = null, timeoutMs = 12_000, extraEnv = {}) {
    // A lingering host from the previous test poisons the port.
    await waitForPortFree(PORT);
    const args = [...extraArgs];
    const env  = { ...testEnv(), ...extraEnv };
    let proc;
    if (stdinBuf !== null) {
        // Pipe mode: feed bytes over stdin.
        proc = spawn(BINARY, args, {
            stdio: ['pipe', 'pipe', 'pipe'],
            detached: true,
            env,
        });
    } else {
        // Terminal mode: allocate a PTY via script(1) so isatty(stdin) is true.
        // stdin stays open as a pipe; script forwards writes to the PTY, so
        // tests can type into the host's local terminal.
        proc = spawn('script', ['-qefc', [BINARY, ...args].join(' '), '/dev/null'], {
            stdio: ['pipe', 'pipe', 'pipe'],
            detached: true,
            env,
        });
    }

    let stdoutBuf = '';
    let roomCode  = '';
    let hostReady = false;

    const ready = new Promise((resolve, reject) => {
        const timer = setTimeout(() => {
            // Kill the half-started host so it cannot leak and hold the port.
            try { process.kill(-proc.pid, 'SIGKILL'); }
            catch { try { proc.kill('SIGKILL'); } catch { /* already gone */ } }
            reject(new Error(`Host startup timeout after ${timeoutMs}ms`));
        }, timeoutMs);

        proc.stdout.on('data', data => {
            stdoutBuf += data.toString();
            const m = stdoutBuf.match(/Room:\s*([a-z]+-[a-z]+-[a-z]+-\d{5})/i);
            if (m && !roomCode) roomCode = m[1].trim();
            if (stdoutBuf.includes('Server listening on port')) {
                clearTimeout(timer);
                hostReady = true;
                resolve();
            }
        });
        proc.stderr.on('data', d => process.stderr.write('[host err] ' + d));
        proc.on('exit', code => {
            clearTimeout(timer);
            if (!hostReady) {
                reject(new Error(
                    `Host exited before startup (code ${code}).\nstdout:\n${stdoutBuf}`));
            }
        });
    });

    if (stdinBuf !== null) {
        proc.stdin.write(stdinBuf);
        proc.stdin.end();
    }

    await ready;
    return { proc, roomCode, getStdout: () => stdoutBuf };
}

/** Polls until predicate() is truthy or the timeout expires. */
async function waitUntil(predicate, timeoutMs = 10_000, stepMs = 200) {
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
        if (predicate()) return true;
        await wait(stepMs);
    }
    return false;
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
 * T-01/02/03  Host basics — one host serves all three checks.
 * Room code format, HTTP + SDP offers, static assets, QR in stdout.
 * The checks are independent reads of the same idle host.
 */
async function testHostBasics() {
    const { proc, roomCode, getStdout } = await spawnHost();
    try {
        // T-01: room code matches word-word-word-NNNNN, words ≤7 chars.
        assert.match(
            roomCode,
            ROOM_RE,
            `Room code "${roomCode}" does not match word-word-word-NNNNN`,
        );
        const [w1, w2, w3] = roomCode.split('-');
        for (const w of [w1, w2, w3]) {
            assert.ok(w.length <= 7, `Word "${w}" is longer than 7 characters`);
        }
        console.log('  [T-01] Room code format ✓', roomCode);

        // T-02: index page.
        const idx = await fetchText(`http://localhost:${PORT}/`);
        assert.strictEqual(idx.status, 200, 'GET / must return 200');
        assert.ok(idx.body.includes('AetherProxy'), 'Index page must contain "AetherProxy"');
        assert.ok(idx.body.includes('xterm.js'),    'Index page must load xterm.js');

        // T-02: SDP offer.
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

        // T-02: static assets are served (non-empty bodies).
        for (const asset of ['/xterm.js', '/xterm.css', '/xterm-addon-fit.js']) {
            const r = await fetchText(`http://localhost:${PORT}${asset}`);
            assert.strictEqual(r.status, 200, `GET ${asset} must return 200`);
            assert.ok(r.body.length > 100, `${asset} body is suspiciously short (${r.body.length} bytes)`);
        }
        console.log('  [T-02] HTTP server + SDP offer ✓');

        // T-03: QR render on stdout. libqrencode emits Unicode half-block
        // characters; some builds draw modules with ESC[7m reverse video.
        const qrShown = await waitUntil(() =>
            /[▀-▟█]/.test(getStdout()) || getStdout().includes('\x1b[7m'), 3000);
        assert.ok(
            qrShown,
            'stdout should contain QR code block characters (▄/█) or ANSI reverse-video QR pattern',
        );
        console.log('  [T-03] QR code in output ✓');
    } finally {
        await killHost(proc);
    }
}

/**
 * T-14  Config subcommand
 * Exercises config set/get/list/unset in an isolated HOME, then asserts
 * a saved port is actually used by the host.
 */
async function testConfigCommand() {
    const home = fs.mkdtempSync(path.join(os.tmpdir(), 'aether-home-'));
    const env  = { ...process.env, HOME: home };
    const run  = (args, opts = {}) =>
        execFileSync(BINARY, args, { env, encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'], ...opts });

    run(['config', 'set', 'port', '9091']);
    assert.strictEqual(run(['config', 'get', 'port']).trim(), '9091', 'get must return the saved value');

    run(['config', 'set', 'no-turn', 'true']);
    run(['config', 'set', 'offline', 'true']);
    run(['config', 'set', 'stun', 'stun:one.example:3478,stun:two.example:3478']);
    const list = run(['config', 'list']);
    assert.ok(list.includes('port=9091'),   'list must show port');
    assert.ok(list.includes('no-turn=true'), 'list must show no-turn');
    assert.ok(list.includes('offline=true'), 'list must show offline');
    assert.ok(list.includes('stun=stun:one.example:3478,stun:two.example:3478'), 'list must show stun');

    let badKeyFailed = false;
    try { run(['config', 'set', 'bogus', '1']); } catch { badKeyFailed = true; }
    assert.ok(badKeyFailed, 'setting an unknown key must exit non-zero');

    run(['config', 'unset', 'port']);
    let getFailed = false;
    try { run(['config', 'get', 'port']); } catch { getFailed = true; }
    assert.ok(getFailed, 'get after unset must exit non-zero');

    // A saved port must drive the host with no --port flag.
    run(['config', 'set', 'port', '9092']);
    run(['config', 'set', 'offline', 'true']);
    const proc = spawn(BINARY, [], { stdio: ['pipe', 'pipe', 'pipe'], env, detached: true });
    let out = '';
    proc.stdout.on('data', d => (out += d.toString()));
    const usedSavedPort = await waitUntil(() => out.includes('Server listening on port 9092'), 8000);
    await killHost(proc);
    assert.ok(usedSavedPort, 'host must listen on the port saved via config set');

    console.log('  [T-14] Config subcommand ✓');
}

/**
 * T-15  Offline mode
 * Uses a fresh HOME with no offline config so the behavior comes from
 * the --offline flag alone (the one per-run flag). Asserts local SDP
 * offers still work and no signaling connection is reported.
 */
async function testOfflineMode() {
    await waitForPortFree(PORT);
    const home = fs.mkdtempSync(path.join(os.tmpdir(), 'aether-home-t15-'));
    const env  = { ...process.env, HOME: home };
    execFileSync(BINARY, ['config', 'set', 'port', String(PORT)], { env });

    const proc = spawn(BINARY, ['--offline'], { stdio: ['pipe', 'pipe', 'pipe'], env, detached: true });
    let out = '';
    proc.stdout.on('data', d => (out += d.toString()));
    try {
        const started = await waitUntil(() => out.includes('Server listening on port'), 8000);
        assert.ok(started, 'offline host must start its HTTP server');
        const offerRes = await fetchText(`http://localhost:${PORT}/offer?id=t15-client`);
        assert.strictEqual(offerRes.status, 200, 'GET /offer must return 200 in offline mode');
        assert.ok(offerRes.body.startsWith('v=0'), 'offline offer must be valid SDP');
        assert.ok(
            !out.includes('Signaling connected'),
            'offline host must not connect to signaling',
        );
        console.log('  [T-15] Offline mode ✓');
    } finally {
        await killHost(proc);
    }
}

/**
 * T-23  Asciicast recording.
 *
 * --record writes an asciicast v2 file: a JSON header line with
 * dimensions, then [offset, "o", data] event lines that capture
 * shell output in order.
 */
async function testAsciicastRecording() {
    const castPath = path.join(fs.mkdtempSync(path.join(os.tmpdir(), 'aether-cast-')), 'session.cast');
    const { proc, getStdout } = await spawnHost(['--record', castPath]);
    try {
        assert.ok(
            await waitUntil(() => getStdout().includes('Recording to')),
            'Host must announce the recording',
        );
        await wait(1000);
        proc.stdin.write('echo cast_$((70+7))\r');
        assert.ok(await waitUntil(() => getStdout().includes('cast_77')), 'Shell output must echo');
        await killHost(proc);

        const lines = fs.readFileSync(castPath, 'utf8').trim().split('\n');
        assert.ok(lines.length >= 2, 'Cast file must have a header and events');
        const header = JSON.parse(lines[0]);
        assert.strictEqual(header.version, 2, 'Header version must be 2');
        assert.ok(header.width > 0 && header.height > 0, 'Header must carry dimensions');
        let output = '';
        let lastOffset = -1;
        for (const line of lines.slice(1)) {
            const ev = JSON.parse(line);
            assert.ok(Array.isArray(ev) && ev.length === 3, 'Events must be triples');
            assert.ok(typeof ev[0] === 'number' && ev[0] >= lastOffset, 'Offsets must not decrease');
            assert.strictEqual(ev[1], 'o', 'Only output events expected');
            lastOffset = ev[0];
            output += ev[2];
        }
        assert.ok(output.includes('cast_77'), 'Recording must contain the shell output');

        console.log('  [T-23] Asciicast recording ✓ —', lines.length - 1, 'events');
    } finally {
        await killHost(proc);
    }
}

// ################ BROWSER-SIDE binary tests ################

/**
 * Terminal session suite — one host, one connected page, ordered checks.
 *
 * Merges the old T-17 landing, T-06…T-12 DOM suite, T-05 resize,
 * T-13 bridge, T-18 identity, T-20 telemetry, and T-19 server exit.
 * All of them exercise the same terminal-mode host over one WebRTC
 * negotiation. Server exit stays last because it kills the host.
 */
async function testTerminalSession(browser, browserName) {
    const { proc, getStdout } = await spawnHost();
    const context = await browser.newContext({ viewport: { width: 1200, height: 800 } });
    const page    = await context.newPage();

    try {
        // Frame capture feeds the T-05 resize checks. The saved name
        // feeds the T-18 identity check. Both must precede goto.
        await page.addInitScript(() => {
            window._aether_sentFrames = [];
            window._aether_dcOpen     = false;
            try { localStorage.setItem('aether-name', 'tester-name'); } catch (e) {}

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

        // T-17: landing page shows without a room fragment, then joins.
        await page.goto(`http://localhost:${PORT}/`);
        assert.ok(await page.isVisible('#landing'), 'Landing must show without a room fragment');
        assert.ok(await page.isVisible('#room-input'), 'Landing must offer a room input');
        await page.fill('#room-input', 'fox-river-stone-48291');
        await page.click('#room-go');
        await page.waitForFunction(
            () => document.getElementById('status-text').textContent === 'Connected',
            { timeout: 15_000 },
        );
        assert.ok(await page.isHidden('#landing'), 'Landing must hide after entering a room');
        console.log(`  [T-17] Landing page (${browserName}) ✓`);

        // T-06: page title.
        const title = await page.title();
        assert.ok(title.includes('AetherProxy'), `Title: "${title}"`);
        console.log('  [T-06] Page title ✓');

        // T-07: xterm terminal mounts (xterm 5.x uses the DOM renderer by default).
        const screen = await page.$('.xterm .xterm-screen');
        assert.ok(screen !== null, 'xterm screen element must be in DOM');
        console.log('  [T-07] xterm mounts ✓');

        // T-08: toolbar buttons present.
        for (const id of ['btn-ctrl','btn-alt','btn-tab','btn-esc',
                           'btn-up','btn-down','btn-left','btn-right']) {
            assert.ok(await page.$(`#${id}`) !== null, `#${id} must be in DOM`);
        }
        console.log('  [T-08] Toolbar buttons ✓');

        // T-09: mode badge hidden by default (terminal mode).
        const badgeDisplay = await page.$eval('#mode-badge',
            el => getComputedStyle(el).display);
        assert.strictEqual(badgeDisplay, 'none', 'Mode badge must be hidden in terminal mode');
        console.log('  [T-09] Mode badge hidden ✓');

        // T-10: Ctrl modifier toggle (not a no-op).
        await page.click('#btn-ctrl');
        assert.ok(await page.$('#btn-ctrl.modifier-active') !== null,
            '#btn-ctrl must gain modifier-active class on click');
        await page.click('#btn-ctrl');
        assert.strictEqual(await page.$('#btn-ctrl.modifier-active'), null,
            '#btn-ctrl must lose modifier-active class on second click');
        console.log('  [T-10] Ctrl modifier toggle ✓');

        // T-11: terminal is interactive in terminal mode (no pipe-locked class).
        const isLocked = await page.$eval('#terminal-container',
            el => el.classList.contains('pipe-locked'));
        assert.strictEqual(isLocked, false, 'Terminal must not be locked in terminal mode');
        console.log('  [T-11] Keyboard not locked in terminal mode ✓');

        // T-12: window.isPipeMode is false in terminal mode.
        const pipeMode = await page.evaluate(() => window.isPipeMode ?? false);
        assert.strictEqual(pipeMode, false, 'window.isPipeMode must be false in terminal mode');
        console.log('  [T-12] window.isPipeMode === false ✓');

        // T-05: resize frame on open, then one per viewport resize.
        await page.waitForFunction(() => window._aether_dcOpen === true, { timeout: 15_000 });
        const framesAtOpen = await page.evaluate(() => window._aether_sentFrames.length);
        await page.setViewportSize({ width: 900, height: 600 });
        await wait(500); // allow fitAddon.fit() + sendResizeFrame() to fire

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
        const last = JSON.parse(resizeFrames[resizeFrames.length - 1].substring(8));
        assert.ok(last.cols > 0 && last.cols < 1000, `cols=${last.cols} out of range`);
        assert.ok(last.rows > 0 && last.rows < 500,  `rows=${last.rows} out of range`);
        const openFrames = frames.slice(0, framesAtOpen).filter(f =>
            typeof f === 'string' &&
            f.startsWith('\x00AETHER:') &&
            f.includes('"type":"resize"')
        );
        assert.ok(
            openFrames.length > 0,
            'Expected a resize frame sent immediately on DataChannel open',
        );
        console.log(`  [T-05] Terminal resize ✓ — cols=${last.cols} rows=${last.rows}`);

        // T-13: local terminal bridge, both directions.
        proc.stdin.write('echo local_bridge_$((20+3))\r');
        const localOk = await waitUntil(() => getStdout().includes('local_bridge_23'));
        assert.ok(localOk, 'Local keystrokes must reach the shared shell and echo locally');

        await page.click('#terminal-container');
        await page.evaluate(() => document.querySelector('.xterm-helper-textarea')?.focus());
        await wait(300);
        await page.keyboard.type('echo browser_bridge_$((40+2))');
        await page.keyboard.press('Enter');
        const remoteOk = await waitUntil(() => getStdout().includes('browser_bridge_42'));
        assert.ok(remoteOk, 'Browser keystrokes must echo on the host terminal');
        console.log('  [T-13] Local terminal bridge ✓');

        // T-18: the saved name rides /offer and returns in presence frames.
        await page.waitForFunction(
            () => document.getElementById('peers-bar').textContent.includes('tester-name'),
            { timeout: 10_000 },
        );
        console.log('  [T-18] Client identity in presence ✓');

        // T-20: the 2000ms getStats() poll reveals the telemetry overlay.
        await page.waitForFunction(
            () => {
                const el = document.getElementById('telemetry');
                return el && el.classList.contains('visible') && el.textContent.length > 0;
            },
            { timeout: 10_000 },
        );
        const telemetry = await page.$eval('#telemetry', el => el.textContent);
        console.log(`  [T-20] Telemetry overlay ✓ — "${telemetry}"`);

        // T-19: SIGTERM broadcasts server_exit. Signal only this test's
        // binary (script's direct child), not the wrapper and not
        // unrelated aetherproxy processes on the machine.
        execFileSync('pkill', ['-TERM', '-x', '-P', String(proc.pid), 'aetherproxy']);
        await page.waitForFunction(
            () => document.getElementById('status-text').textContent === 'Server terminated',
            { timeout: 10_000 },
        );
        const exitLocked = await page.$eval('#terminal-container',
            el => el.classList.contains('pipe-locked'));
        assert.ok(exitLocked, 'Input must lock after server_exit');
        console.log(`  [T-19] Server exit broadcast (${browserName}) ✓`);
    } finally {
        await context.close();
        await killHost(proc);
    }
}

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
async function testPipeMode(browser, browserName) {
    const PIPE_BYTES = 1000;
    const payload    = Buffer.alloc(PIPE_BYTES, 0x41); // 1000 × 'A'

    const { proc, roomCode } = await spawnHost([], payload);

    // Give the binary time to start the HTTP server even though stdin is pipe.
    await wait(800);

    const context = await browser.newContext();
    const page    = await context.newPage();

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

        await page.goto(`http://localhost:${PORT}/#fox-river-stone-48291`);

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
        await context.close();
        await killHost(proc);
    }
}

/**
 * T-16  Interactive client lifecycle.
 *
 * `aetherproxy interactive <room>` is a client, not a server. It serves
 * the web client on a loopback port, opens the browser ($BROWSER
 * honoured), and exits once the tab closes. The page reaches a separate
 * host through a local signaling relay. The host must survive the
 * client leaving.
 */
async function testInteractiveLifecycle(browser, browserName) {
    const SIG_PORT = PORT + 4;

    // Non-offline HOME wired to the local signaling relay.
    const home = fs.mkdtempSync(path.join(os.tmpdir(), 'aether-home-t16-'));
    const env  = { ...process.env, HOME: home };
    execFileSync(BINARY, ['config', 'set', 'port', String(PORT)], { env });
    execFileSync(BINARY, ['config', 'set', 'signal', `ws://127.0.0.1:${SIG_PORT}`], { env });
    execFileSync(BINARY, ['config', 'set', 'no-stun', 'true'], { env });
    execFileSync(BINARY, ['config', 'set', 'no-turn', 'true'], { env });
    execFileSync(BINARY, ['config', 'set', 'admit', 'false'], { env });

    const sig = spawn('node', ['signaling/server.js'], {
        stdio: ['ignore', 'pipe', 'pipe'],
        env: {
            ...process.env,
            PORT: String(SIG_PORT),
            NODE_PATH: path.join(__dirname, 'node_modules'),
        },
    });
    let sigOut = '';
    sig.stdout.on('data', d => (sigOut += d.toString()));
    sig.stderr.on('data', d => process.stderr.write('[sig err] ' + d));

    let host = null;
    let inter = null;
    let context = null;
    try {
        assert.ok(
            await waitUntil(() => sigOut.includes('running on port')),
            'Signaling relay must start',
        );

        // Server side: a normal terminal-mode host.
        host = await spawnHost([], null, 12_000, { HOME: home });
        assert.match(host.roomCode, ROOM_RE, 'Host must print a room code');

        // Client side: interactive launcher joining the host room.
        inter = spawn(BINARY, ['interactive', host.roomCode], {
            stdio: ['ignore', 'pipe', 'pipe'],
            detached: true,
            env: { ...env, BROWSER: '/bin/true' },
        });
        let interOut = '';
        let interExited = false;
        inter.stdout.on('data', d => (interOut += d.toString()));
        inter.on('exit', () => { interExited = true; });

        const urlRe = /Opening browser: (http:\/\/127\.0\.0\.1:\d+\/\S+)/;
        assert.ok(
            await waitUntil(() => urlRe.test(interOut)),
            'Interactive must print the local browser URL',
        );
        const url = interOut.match(urlRe)[1];
        assert.ok(!interOut.includes('Room:'), 'Interactive must not create its own room');

        context = await browser.newContext();
        const page = await context.newPage();
        await page.goto(url);
        await page.waitForFunction(
            () => document.getElementById('status-text').textContent === 'Connected',
            { timeout: 20_000 },
        );

        // Keystrokes reach the host shell through the relay.
        await page.click('#terminal-container');
        await page.evaluate(() => document.querySelector('.xterm-helper-textarea')?.focus());
        await wait(300);
        await page.keyboard.type('echo interactive_bridge_$((50+6))');
        await page.keyboard.press('Enter');
        assert.ok(
            await waitUntil(() => host.getStdout().includes('interactive_bridge_56')),
            'Browser keystrokes must reach the host shell',
        );

        // $USER identity flows through the hello payload into presence.
        const expectedName = process.env.USER || process.env.LOGNAME || 'guest';
        await page.waitForFunction(
            (n) => document.getElementById('peers-bar').textContent.includes(n),
            expectedName,
            { timeout: 10_000 },
        );

        // Closing the tab ends the launcher, not the host.
        await page.goto('about:blank');
        await context.close();
        context = null;
        assert.ok(
            await waitUntil(() => interExited, 20_000),
            'Interactive must exit after the tab closes',
        );
        assert.ok(
            host.proc.exitCode === null && host.proc.signalCode === null,
            'Host must stay alive after the interactive client leaves',
        );

        console.log(`  [T-16] Interactive client lifecycle (${browserName}) ✓`);
    } finally {
        if (context) await context.close();
        if (inter) await killHost(inter);
        if (host) await killHost(host.proc);
        sig.kill('SIGKILL');
    }
}

/**
 * T-21  Input debounce and lock indicator.
 *
 * Two collaborators type at once. The second client's input inside the
 * 500ms debounce window is dropped server-side. The host sends a locked
 * frame and the second client renders the lock badge with the writer's
 * name. After the window the second client takes over.
 */
async function testInputDebounce(browser, browserName) {
    const { proc, getStdout } = await spawnHost();
    const ctxA = await browser.newContext();
    const ctxB = await browser.newContext();
    const pageA = await ctxA.newPage();
    const pageB = await ctxB.newPage();
    try {
        await pageA.addInitScript(() => {
            try { localStorage.setItem('aether-name', 'alice'); } catch (e) {}
        });
        await pageB.addInitScript(() => {
            try { localStorage.setItem('aether-name', 'bobby'); } catch (e) {}
        });
        for (const page of [pageA, pageB]) {
            await page.goto(`http://localhost:${PORT}/#fox-river-stone-48291`);
            await page.waitForFunction(
                () => document.getElementById('status-text').textContent === 'Connected',
                { timeout: 15_000 },
            );
            await page.click('#terminal-container');
            await page.evaluate(() => document.querySelector('.xterm-helper-textarea')?.focus());
        }
        await wait(500);

        // A types first and becomes the writer.
        await pageA.keyboard.type('AAA');
        assert.ok(
            await waitUntil(() => getStdout().includes('AAA'), 5_000),
            'Writer input must reach the host',
        );

        // B types inside the debounce window: dropped, badge shows.
        await pageB.keyboard.type('BBB');
        await pageB.waitForFunction(
            () => document.getElementById('lock-badge').classList.contains('visible') &&
                  document.getElementById('lock-badge-text').textContent.includes('alice'),
            { timeout: 3_000 },
        );
        await wait(700);
        assert.ok(!getStdout().includes('BBB'), 'Debounced input must not reach the host');

        // The window expired. B takes over as writer.
        await pageB.keyboard.type('CCC');
        assert.ok(
            await waitUntil(() => getStdout().includes('CCC'), 5_000),
            'Input must pass after the debounce window',
        );

        console.log(`  [T-21] Input debounce + lock indicator (${browserName}) ✓`);
    } finally {
        await ctxA.close();
        await ctxB.close();
        await killHost(proc);
    }
}

/**
 * T-22  Admission gate.
 *
 * With admit=true the host prompts before linking input. The pending
 * client sees output but its keystrokes drop, and the badge reads
 * "Waiting for approval". 'y' promotes it to collaborator. A second
 * client answered with 'n' is kicked with an error frame.
 */
async function testAdmissionGate(browser, browserName) {
    await waitForPortFree(PORT);
    const home = fs.mkdtempSync(path.join(os.tmpdir(), 'aether-home-t22-'));
    const env  = { ...process.env, HOME: home };
    execFileSync(BINARY, ['config', 'set', 'port', String(PORT)], { env });
    execFileSync(BINARY, ['config', 'set', 'offline', 'true'], { env });
    execFileSync(BINARY, ['config', 'set', 'admit', 'true'], { env });

    const { proc, getStdout } = await spawnHost([], null, 12_000, { HOME: home });
    const ctxA = await browser.newContext();
    const pageA = await ctxA.newPage();
    let ctxB = null;
    try {
        await pageA.addInitScript(() => {
            try { localStorage.setItem('aether-name', 'alice'); } catch (e) {}
        });
        await pageA.goto(`http://localhost:${PORT}/#fox-river-stone-48291`);
        await pageA.waitForFunction(
            () => document.getElementById('status-text').textContent === 'Connected',
            { timeout: 15_000 },
        );

        // Host prompts and the client shows the pending badge.
        assert.ok(
            await waitUntil(() => getStdout().includes('Connection request from alice')),
            'Host must prompt for admission',
        );
        await pageA.waitForFunction(
            () => document.getElementById('mode-badge-text').textContent.includes('approval'),
            { timeout: 5_000 },
        );

        // Pending keystrokes never reach the shell. The container is
        // pointer-locked while pending, so focus the textarea directly.
        await pageA.evaluate(() => document.querySelector('.xterm-helper-textarea')?.focus());
        await pageA.keyboard.type('PENDINGXYZ');
        await wait(700);
        assert.ok(!getStdout().includes('PENDINGXYZ'), 'Pending input must not reach the host');

        // Approve. The badge clears and input flows.
        proc.stdin.write('y');
        await pageA.waitForFunction(
            () => !document.getElementById('mode-badge').classList.contains('visible'),
            { timeout: 5_000 },
        );
        await pageA.keyboard.type('echo admit_$((30+3))');
        await pageA.keyboard.press('Enter');
        assert.ok(
            await waitUntil(() => getStdout().includes('admit_33')),
            'Approved input must reach the shell',
        );

        // A second client gets rejected and kicked.
        ctxB = await browser.newContext();
        const pageB = await ctxB.newPage();
        await pageB.addInitScript(() => {
            try { localStorage.setItem('aether-name', 'mallory'); } catch (e) {}
        });
        await pageB.goto(`http://localhost:${PORT}/#fox-river-stone-48291`);
        assert.ok(
            await waitUntil(() => getStdout().includes('Connection request from mallory')),
            'Host must prompt for the second client',
        );
        proc.stdin.write('n');
        await pageB.waitForFunction(
            () => {
                const t = document.getElementById('status-text').textContent;
                return t.includes('rejected') || t === 'Disconnected';
            },
            { timeout: 10_000 },
        );

        console.log(`  [T-22] Admission gate (${browserName}) ✓`);
    } finally {
        await ctxA.close();
        if (ctxB) await ctxB.close();
        await killHost(proc);
    }
}

// ################ test registry and runner ################

const BINARY_TESTS = [
    { name: 'T-01/02/03  Host basics',   fn: testHostBasics       },
    { name: 'T-14  Config subcommand',   fn: testConfigCommand    },
    { name: 'T-15  Offline mode',        fn: testOfflineMode      },
    { name: 'T-23  Asciicast recording', fn: testAsciicastRecording },
];

// Each test gets a fresh context from a shared browser instance.
// Launching the browser once per engine saves ~1s per test.
const BROWSER_TESTS = [
    { name: 'Terminal session suite',              fn: testTerminalSession },
    { name: 'T-04  Pipe mode',                     fn: testPipeMode },
    { name: 'T-16  Interactive client lifecycle',  fn: testInteractiveLifecycle },
    { name: 'T-21  Input debounce',                fn: testInputDebounce },
    { name: 'T-22  Admission gate',                fn: testAdmissionGate },
];

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
    // BROWSERS=chromium narrows the matrix (used by sanitizer runs).
    const wanted = (process.env.BROWSERS || 'chromium,firefox').toLowerCase().split(',');
    const matrix = [[chromium, 'Chromium'], [firefox, 'Firefox']]
        .filter(([, name]) => wanted.includes(name.toLowerCase()));
    for (const [browserType, browserName] of matrix) {
        console.log(`\n╔══════════════════════════════════════╗`);
        console.log(`║  Browser tests — ${browserName.padEnd(18)} ║`);
        console.log(`╚══════════════════════════════════════╝`);

        const browser = await browserType.launch({ headless: true });
        try {
            for (const t of BROWSER_TESTS) {
                const label = `${t.name} (${browserName})`;
                process.stdout.write(`\n  Running: ${label} … \n`);
                try {
                    await t.fn(browser, browserName);
                    results.push({ name: label, ok: true });
                } catch (e) {
                    console.error(`  ✗ ${e.message}`);
                    results.push({ name: label, ok: false, err: e.message });
                }
            }
        } finally {
            await browser.close();
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
