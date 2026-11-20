// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const path = require('node:path');
const source = fs.readFileSync(path.join(__dirname, '../src/providers/tiktok/TikTokLiveChat.cpp'), 'utf8');
const script = source.match(/R"JS\(([\s\S]*?)\)JS"/)[1];
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));

function fixture({ liveRoom = { status: 2, userCount: 7 }, pageStatus = 2 } = {}) {
  const state = { originalCalls: 0, current: null, hasRoot: true, rows: [] };
  const makeEvents = () => ({
    pbReader: { decodePayload: (_, payload) => Promise.resolve(payload) },
    runCallbackByMethod() { state.originalCalls++; return 42; }
  });
  state.current = makeEvents();
  state.makeEvents = makeEvents;
  const rootNode = { parentElement: null };
  Object.defineProperty(rootNode, '__reactFiber$test', { enumerable: true, get: () => ({
    memoizedProps: { value: { imInstance: { _messageEvents: state.current } } }
  }) });
  const context = vm.createContext({
    window: {},
    document: {
      title: 'LIVE fixture',
      getElementById: () => ({ textContent: JSON.stringify({
        CurrentRoom: { roomId: '123', showLiveChat: true, enableChat: true,
          roomInfo: { liveRoom } },
        LiveRoom: { liveRoomStatus: pageStatus }
      }) }),
      querySelector: () => state.hasRoot ? rootNode : null,
      querySelectorAll: () => state.rows
    }
  });
  state.snapshot = () => JSON.parse(vm.runInContext(script, context));
  state.context = context;
  state.send = async (method, payload, id = 'message-1', events = state.current) => {
    const result = events.runCallbackByMethod(method, { payload, msg_id: id });
    await delay(0);
    return result;
  };
  return state;
}

async function regressions() {
  let passed = 0;
  async function test(name, run) { await run(); passed++; console.log('PASS', name); }
  const chat = { user: { nickname: 'Test User', unique_id: 'testuser' }, content: 'hello from an unrendered chat' };
  await test('A zero page placeholder cannot hide the actual live room status', async () => {
    const f = fixture({ pageStatus: 0 });
    const snapshot = f.snapshot();
    assert.equal(snapshot.liveRoomStatus, 2);
    assert.equal(snapshot.viewerCount, 7);
  });
  await test('An ended room overrides a stale live page flag', async () => {
    const f = fixture({ liveRoom: { status: 4 }, pageStatus: 2 });
    assert.equal(f.snapshot().liveRoomStatus, 4);
  });
  await test('Chat is delivered when the rendered message list is empty', async () => {
    const f = fixture();
    assert.equal(f.snapshot().chatTapInstalled, false);
    assert.equal(await f.send('WebcastChatMessage', chat), 42);
    const snapshot = f.snapshot();
    assert.equal(snapshot.messages.length, 0);
    assert.equal(snapshot.chatEvents.length, 1);
    assert.equal(snapshot.chatEvents[0].text, chat.content);
    assert.equal(snapshot.chatEvents[0].id, 'message-1');
    assert.equal(snapshot.chatEvents[0].uniqueId, 'testuser');
    assert.equal(snapshot.chatTapInstalled, true);
    assert.equal(f.snapshot().chatEvents.length, 0);
    assert.equal(f.originalCalls, 1);
  });
  await test('Repeated snapshots do not stack callback wrappers', async () => {
    const f = fixture(); f.snapshot();
    const callback = f.current.runCallbackByMethod;
    for (let i = 0; i < 5; i++) f.snapshot();
    assert.equal(f.current.runCallbackByMethod, callback);
    await f.send('WebcastChatMessage', chat);
    assert.equal(f.snapshot().chatEvents.length, 1);
    assert.equal(f.originalCalls, 1);
  });
  await test('A replacement TikTok connection is attached and old callbacks are ignored', async () => {
    const f = fixture(); f.snapshot();
    const old = f.current;
    f.current = f.makeEvents(); f.snapshot();
    await f.send('WebcastChatMessage', chat, 'old', old);
    await f.send('WebcastChatMessage', chat, 'new');
    const snapshot = f.snapshot();
    assert.deepEqual(snapshot.chatEvents.map(m => m.id), ['new']);
    assert.equal(f.originalCalls, 2);
  });
  await test('Temporarily missing React context recovers without duplicate events', async () => {
    const f = fixture(); f.snapshot();
    f.hasRoot = false; assert.equal(f.snapshot().chatTapInstalled, false);
    f.hasRoot = true; f.snapshot();
    await f.send('WebcastChatMessage', chat);
    assert.equal(f.snapshot().chatEvents.length, 1);
    assert.equal(f.originalCalls, 1);
  });
  await test('Viewer counts update independently, including a real zero', async () => {
    const f = fixture(); assert.equal(f.snapshot().viewerCount, 7);
    await f.send('WebcastRoomUserSeqMessage', { total: '369' });
    assert.equal(f.snapshot().viewerCount, 369);
    await f.send('WebcastRoomUserSeqMessage', { total: 0 });
    assert.equal(f.snapshot().viewerCount, 0);
  });
  await test('Initial page statistics supply a count until live events arrive', async () => {
    const f = fixture({ liveRoom: { status: 2, liveRoomStats: { userCount: 401 } } });
    assert.equal(f.snapshot().viewerCount, 401);
    await f.send('WebcastRoomUserSeqMessage', { total: 386 });
    assert.equal(f.snapshot().viewerCount, 386);
  });
  await test('Join and gift delivery are retained', async () => {
    const f = fixture(); f.snapshot();
    await f.send('WebcastMemberMessage', { user: chat.user }, 'join');
    await f.send('WebcastGiftMessage', { user: chat.user, gift: { name: 'Rose', diamond_count: 1 }, repeat_count: 2 }, 'gift');
    const snapshot = f.snapshot();
    assert.equal(snapshot.joinEvents.length, 1);
    assert.equal(snapshot.giftEvents.length, 1);
    assert.equal(snapshot.giftEvents[0].quantity, 2);
  });
  await test('Pending chat storage is bounded during a stalled consumer', async () => {
    const f = fixture(); f.snapshot();
    for (let i = 0; i < 600; i++) await f.send('WebcastChatMessage', chat, String(i));
    const snapshot = f.snapshot();
    assert.equal(snapshot.chatEvents.length, 500);
    assert.equal(snapshot.chatEvents[0].id, '100');
    assert.equal(snapshot.chatEvents.at(-1).id, '599');
  });
  await test('Decoder errors cannot break TikTok callbacks or disable DOM fallback', async () => {
    const f = fixture(); f.snapshot();
    await f.send('WebcastChatMessage', chat);
    assert.equal(f.snapshot().chatTapInstalled, true);
    f.current.pbReader.decodePayload = () => { throw new Error('bad payload'); };
    await f.send('WebcastChatMessage', chat);
    assert.equal(f.snapshot().chatTapInstalled, false);
    assert.equal(f.originalCalls, 2);
  });
  console.log(`${passed} production-script regressions passed`);
}

regressions().catch(error => { console.error(error); process.exitCode = 1; });
