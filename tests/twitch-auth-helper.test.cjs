// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const test = require('node:test');

const source = fs.readFileSync(path.join(__dirname, '../src/providers/twitch/api/Helix.cpp'), 'utf8');
const script = source.match(/R"MERGERINO_JS\(([\s\S]*?)\)MERGERINO_JS"/)[1];
const token = 'synthetic_auth_token_12345678901234567890';
const otherToken = 'unrelated_session_12345678901234567890';
const integrity = 'synthetic_integrity_12345678901234567890';

function fixture(options = {}) {
  const state = { writes: [], logs: [], errors: [], requests: [], gesture: false };
  const element = tag => ({
    tag, children: [], listeners: {}, style: {}, hidden: false,
    append(...children) {
      for (const child of children) {
        child.parent = this;
        this.children.push(child);
      }
    },
    remove() {
      if (this.parent) this.parent.children = this.parent.children.filter(child => child !== this);
    },
    setAttribute(name, value) { this[name] = value; },
    addEventListener(name, listener) { this.listeners[name] = listener; },
    focus() { this.focused = true; },
    select() { this.selected = true; }
  });
  const body = element('body');
  const descendants = node => [node, ...node.children.flatMap(descendants)];
  const document = {
    body, createElement: element,
    getElementById: id => descendants(body).find(node => node.id === id)
  };
  Object.defineProperty(document, 'cookie', { get() {
    if (options.cookieBlocked) throw new Error('Cookie access blocked');
    return options.cookies || '';
  }});
  const storage = values => ({
    getItem: key => values[key] ?? null,
    key: index => Object.keys(values)[index] ?? null,
    get length() { return Object.keys(values).length; }
  });
  const context = vm.createContext({
    location: { protocol: 'https:', hostname: options.hostname || 'www.twitch.tv' },
    document,
    localStorage: storage(options.local || {}),
    sessionStorage: storage(options.session || {}),
    window: {},
    crypto: { randomUUID: () => '00000000-0000-4000-8000-000000000000' },
    navigator: options.clipboardMissing ? {} : { clipboard: { async writeText(value) {
      if (options.clipboardDenied || (options.needsGesture && !state.gesture)) {
        throw new Error('Clipboard write blocked');
      }
      state.writes.push(value);
    }}},
    // Deliberately omit the DevTools copy() function: async console helper
    // results are not delivered to Firefox's clipboard consumer.
    console: {
      log: (...values) => state.logs.push(values.join(' ')),
      warn: (...values) => state.logs.push(values.join(' ')),
      error: (...values) => state.errors.push(values.join(' '))
    },
    AbortController, setTimeout, clearTimeout,
    fetch: async (url, request) => {
      state.requests.push({ url, request });
      if (options.integrityRejected) throw new Error('Network unavailable');
      return { ok: true, json: async () => ({ token: integrity }) };
    }
  });
  if (options.localBlocked) {
    Object.defineProperty(context, 'localStorage', { get() { throw new Error('Storage access blocked'); }});
  }
  state.run = () => vm.runInContext(script, context);
  state.panel = () => document.getElementById('mergerino-twitch-auth-copy');
  state.field = () => descendants(body).find(node => node.tag === 'textarea');
  state.click = async label => {
    const button = descendants(body).find(node => node.tag === 'button' && node.textContent === label);
    assert.ok(button, `Missing ${label} button`);
    state.gesture = true;
    try { await button.listeners.click(); } finally { state.gesture = false; }
  };
  state.payload = () => JSON.parse(state.writes.at(-1));
  return state;
}

test('copies the complete payload after async integrity work without DevTools copy()', async () => {
  const f = fixture({ local: { 'auth-token': token, 'twilight.deviceId': JSON.stringify(otherToken) } });
  await f.run();
  assert.equal(f.writes.length, 1);
  assert.equal(f.payload().token, token);
  assert.equal(f.payload().clientIntegrity, integrity);
  assert.equal(f.payload().deviceId, otherToken);
  assert.equal(f.requests[0].request.headers.Authorization, `OAuth ${token}`);
  assert.equal(f.panel(), undefined);
  assert.ok(f.logs.some(value => value.startsWith('Mergerino token copied.')));
  assert.deepEqual(f.errors, []);
});

for (const [name, input] of [
  ['plain', token], ['JSON quoted', JSON.stringify(token)],
  ['single quoted', `'${token}'`], ['quoted OAuth prefix', JSON.stringify(`OAuth ${token}`)],
  ['nested quoted prefixes', `"oauth:'${token}'"`],
  ['JSON object', JSON.stringify({ id: otherToken, authToken: token })],
  ['nested JSON object', JSON.stringify({ data: { user: { access_token: token } } })]
]) {
  test(`extracts ${name} storage tokens`, async () => {
    const f = fixture({ local: { 'auth-token': input } });
    await f.run();
    assert.equal(f.payload().token, token);
  });
}

test('extracts encoded cookie values with equals signs intact', async () => {
  const padded = token + '==';
  const f = fixture({ cookies: `auth-token=${encodeURIComponent(JSON.stringify(padded))}` });
  await f.run();
  assert.equal(f.payload().token, padded);
});

test('a malformed cookie does not hide a valid local storage token', async () => {
  const f = fixture({ cookies: 'auth-token=%ZZ', local: { 'auth-token': token } });
  await f.run();
  assert.equal(f.payload().token, token);
});

test('an unrelated auth cookie does not hide an explicit storage auth-token', async () => {
  const f = fixture({ cookies: `auth-session=${otherToken}`, local: { 'auth-token': token } });
  await f.run();
  assert.equal(f.payload().token, token);
});

test('an explicit session storage auth-token wins over a generic local storage token', async () => {
  const f = fixture({ local: { token: otherToken }, session: { 'auth-token': token } });
  await f.run();
  assert.equal(f.payload().token, token);
});

test('blocked cookies and local storage still allow session storage', async () => {
  const f = fixture({ cookieBlocked: true, localBlocked: true, session: { 'auth-token': token } });
  await f.run();
  assert.equal(f.payload().token, token);
});

test('nested account metadata is not mistaken for an auth token', async () => {
  const f = fixture({ local: { 'twilight-user': JSON.stringify({ id: otherToken, displayName: otherToken }) } });
  await f.run();
  assert.equal(f.writes.length, 0);
  assert.ok(f.errors.some(value => value.includes('No Twitch auth token found')));
});

test('unknown auth-token keys remain supported without accepting refresh tokens', async () => {
  const f = fixture({ local: { refresh_access_token: otherToken, 'custom.oauthToken': token } });
  await f.run();
  assert.equal(f.payload().token, token);
});

test('integrity failure still permits copying a usable auth token', async () => {
  const f = fixture({ cookies: `auth-token=${token}`, integrityRejected: true });
  await f.run();
  assert.equal(f.payload().token, token);
  assert.equal(f.payload().clientIntegrity, '');
});

test('blocked automatic copying offers a button and only reports success after copying', async () => {
  const f = fixture({ local: { 'auth-token': token }, needsGesture: true });
  await f.run();
  assert.equal(f.writes.length, 0);
  assert.ok(f.panel());
  assert.equal(f.field().hidden, true);
  assert.equal(f.field().value, undefined);
  assert.equal(f.logs.some(value => value.startsWith('Mergerino token copied.')), false);
  await f.click('Copy Token');
  assert.equal(f.payload().token, token);
  assert.equal(f.payload().clientIntegrity, integrity);
  assert.equal(f.panel(), undefined);
  assert.ok(f.logs.some(value => value.startsWith('Mergerino token copied.')));
});

for (const option of ['clipboardDenied', 'clipboardMissing']) {
  test(`${option} allows selecting the complete payload for manual copying`, async () => {
    const f = fixture({ cookies: `auth-token=${token}`, [option]: true });
    await f.run();
    await f.click('Copy Token');
    assert.equal(f.writes.length, 0);
    assert.equal(f.field().hidden, false);
    assert.equal(f.field().readOnly, true);
    assert.equal(f.field().selected, true);
    assert.equal(JSON.parse(f.field().value).clientIntegrity, integrity);
    assert.equal(f.logs.some(value => value.startsWith('Mergerino token copied.')), false);
    await f.click('Close');
    assert.equal(f.panel(), undefined);
  });
}

test('re-running a blocked helper replaces its previous copy panel', async () => {
  const f = fixture({ cookies: `auth-token=${token}`, needsGesture: true });
  await f.run();
  const firstPanel = f.panel();
  await f.run();
  assert.notEqual(f.panel(), firstPanel);
  assert.equal(firstPanel.parent.children.includes(firstPanel), false);
});

test('a successful retry removes the previous blocked copy panel', async () => {
  const options = { cookies: `auth-token=${token}`, clipboardDenied: true };
  const f = fixture(options);
  await f.run();
  assert.ok(f.panel());
  options.clipboardDenied = false;
  await f.run();
  assert.equal(f.payload().token, token);
  assert.equal(f.panel(), undefined);
});

for (const options of [
  {}, { cookies: `auth-token=${'x'.repeat(4097)}` },
  { hostname: 'twitch.tv.example.com', cookies: `auth-token=${token}` }
]) {
  test(`invalid token or origin does not copy data: ${options.hostname || options.cookies ? 'invalid' : 'missing'}`, async () => {
    const f = fixture(options);
    await f.run();
    assert.equal(f.writes.length, 0);
    assert.equal(f.requests.length, 0);
    assert.equal(f.panel(), undefined);
    assert.equal(f.errors.length, 1);
    assert.equal(f.logs.some(value => value.startsWith('Mergerino token copied.')), false);
  });
}
