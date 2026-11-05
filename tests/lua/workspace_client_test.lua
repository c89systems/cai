local app = require('cai.app')
local runtime, workspace = assert(arg[1]), assert(arg[2])
local function config(extra)
  extra = extra or {}
  extra.runtime_directory, extra.workspace = runtime, workspace
  return extra
end
local function rejected(ok, message, code)
  assert(ok == nil and type(message) == 'string' and code ~= 0, message)
end
local function drive(client, predicate)
  local deadline = os.time() + 10
  while not predicate() do
    assert(os.time() <= deadline, 'Lua attachment did not progress')
    assert(client:wait(1000))
  end
end
local function connected(client)
  drive(client, function() return client:get_state().transport.phase == app.WORKSPACE_CONNECTED end)
end
local function detached(client)
  assert(client:detach())
  drive(client, function() return client:get_state().transport.phase == app.WORKSPACE_DISCONNECTED end)
  assert(client:close())
end
local function bound(client)
  assert(client:bind {request_id = '1'})
  drive(client, function() return client:get_state().bound end)
end
-- Strict configuration/identity handling, including exact UINT64 conversions.
rejected(app.client(false))
for _, extra in ipairs {
  {unexpected = true}, {on_event = true}, {expected_instance = ''},
  {expected_instance = string.rep('x', 17)}, {handshake_timeout_ms = -1},
  {handshake_timeout_ms = 1.5}, {handshake_timeout_ms = '18446744073709551616'},
  {backlog_limit = 'not an integer'}, {requirements = false},
  {requirements = {not_an_array = true}},
  {['on_event\0ignored'] = true},
  {requirements = {{name = 'x', digest = 'short'}}},
  {requirements = {{name = 'b', digest = string.rep('x', 32)}, {name = 'a', digest = string.rep('x', 32)}}},
} do rejected(app.client(config(extra))) end

local event_count, catalogs, reentrant = 0, 0, false
local receipts, history_results, controls = {}, {}, {}
local raw_bytes, tool_bytes, replayed_execution = 0, 0, 0
local multi_chunk = false
local data_offsets, history = {}, {}
local recalled, replay_done = false, false
local c
local callbacks = config {
  on_event = function(event)
    event_count = event_count + 1
    assert(type(event.request) == 'string')
    assert(type(event.state.transport.generation) == 'string')
    if not reentrant then
      rejected(c:pump())
      rejected(c:wait(0))
      rejected(c:close())
      rejected(c:detach())
      rejected(c:unsubscribe())
      reentrant = true
    end
    if event.type == app.CLIENT_STATUS then
      assert(event.status.available and not event.status.stale)
      assert(event.status.effective.model and event.status.session_id)
      assert(type(event.status.metrics.turn_elapsed_ms) == 'string')
      assert(c:get_view().revision == event.status.revision)
    elseif event.type == app.CLIENT_INPUT and event.input.type == app.INPUT_OBSERVATION_RESULT then
      receipts[event.input.result.request_id] = event.input.result
    elseif event.type == app.CLIENT_HISTORY_RESULT then
      history_results[event.request] = event.input.result
    elseif event.type == app.CLIENT_CONTROL_RESULT then
      controls[event.request] = event.input.result
    end
  end,
  on_catalog = function(event)
    assert(type(event.revision) == 'string' and type(event.index) == 'string')
    if event.phase == app.CATALOG_END then catalogs = catalogs + 1 end
  end,
  on_transcript = function(event)
    assert(type(event.sequence) == 'string' and type(event.position) == 'string')
    if event.kind == app.TRANSCRIPT_EVENT_BEGIN then data_offsets[event.sequence] = 0
    elseif event.kind == app.TRANSCRIPT_DATA then
      assert(event.offset == tostring(data_offsets[event.sequence]))
      assert(event.length == #event.data)
      data_offsets[event.sequence] = data_offsets[event.sequence] + event.length
      raw_bytes = raw_bytes + event.length
    elseif event.kind == app.TRANSCRIPT_REPLAY_END then replay_done = true end
  end,
  on_execution = function(event)
    assert(event.code == 0 and event.record and event.boundary)
    local r = event.record
    assert(r.kind and r.session_id and type(r.source_sequence) == 'string')
    assert(#r.data <= 4096)
    if r.tool_name == 'exec_command' and r.offset ~= '0' then multi_chunk = true end
    tool_bytes = tool_bytes + #r.data
    if event.replay then replayed_execution = replayed_execution + 1 end
  end,
  on_prompt_history = function(event)
    if event.kind == 1 then history[#history + 1] = ''
    elseif event.kind == 2 then history[#history] = history[#history] .. event.data
    elseif event.kind == 4 then recalled = true end
  end,
}
c = assert(app.client(callbacks))
callbacks.on_event = function() error('configuration callback mutation escaped') end
assert(c:get_view() == nil and c:directory() == workspace)
local interests = assert(c:prepare())
assert(interests.fd >= 0 and type(interests.events) == 'number' and type(interests.deadline_ms) == 'string')
connected(c)
drive(c, function() return c:get_view() and catalogs > 0 end)
assert(reentrant and event_count > 0)
bound(c)
local initial_view = c:get_view()
local s = c:get_state()
assert(s.transport.controller == s.transport.attachment)
assert(s.producer ~= '0' and s.conversation ~= '0')
assert(c:subscribe {request_id = '1', session = s.session, conversation = s.conversation, replay_turns = 0})
drive(c, function() return replay_done end)

for _, value in ipairs {'18446744073709551616', '-1', '-0', '1x', '1\0', -1, 1.5, true} do
  rejected(c:request {operation = 'synchronize', request_id = value,
    conversation = s.conversation, generation = s.transport.generation})
end
rejected(c:request {operation = 'forget_origin', request_id = '1', conversation = s.conversation, generation = s.transport.generation})
rejected(c:request {operation = 'offer', request_id = '1', conversation = s.conversation, generation = s.transport.generation, text = 'bad\0text'})
rejected(c:subscribe {request_id = '2', session = s.session, replay_turns = -2})
rejected(c:prompt_history {request_id = '2', session = s.session, limit = '4294967296'})
rejected(c:wait(-2)); rejected(c:pump(-1))
for _, method in ipairs {'bind', 'request', 'takeover', 'shutdown', 'cancel', 'save_prompt', 'subscribe', 'prompt_history'} do
  rejected(c[method](c, false))
end

-- Saving recall is not a model turn; result IDs are separate from input IDs.
local draft = 'LUA UNSENT DRAFT ' .. string.rep('λ🚀', 1200)
assert(c:save_prompt {request_id = '1', generation = s.transport.generation, text = draft})
drive(c, function() return history_results['1'] end)
assert(history_results['1'].code == 0 and history_results['1'].item_id ~= '0')
assert(c:prompt_history {request_id = '2', conversation = s.conversation, session = s.session})
drive(c, function() return recalled end)
assert(#history == 1 and history[1] == draft)

-- Held entries, native edit tokens, snapshots/deltas and explicit promotion.
local function send(operation, id, extra)
  extra = extra or {}
  extra.operation, extra.request_id = operation, tostring(id)
  extra.conversation, extra.generation = s.conversation, s.transport.generation
  assert(c:request(extra))
  drive(c, function() return receipts[tostring(id)] end)
  assert(receipts[tostring(id)].outcome == 0, receipts[tostring(id)].reason)
  return receipts[tostring(id)]
end
local offered = send('offer', 1, {text = 'HELD LUA PROMPT', enabled = true})
local edit = send('begin_edit', 2, {item_id = offered.item_id, item_revision = offered.item_revision})
local edited = send('end_edit', 3, {item_id = edit.item_id, item_revision = edit.item_revision, edit_token = edit.edit_token, text = 'LUA SOCKET PROMPT'})
send('approve', 4, {item_id = edited.item_id, item_revision = edited.item_revision})
drive(c, function()
  local v = c:get_view()
  return v and v.metrics.has_last_turn and not v.metrics.turn_active
end)
assert(raw_bytes > 8192 and tool_bytes > 8192 and multi_chunk,
  ('streamed raw=%d execution=%d multichunk=%s'):format(raw_bytes, tool_bytes, tostring(multi_chunk)))
assert(not initial_view.metrics.has_last_turn)

-- A second client is an observer until explicit takeover. Failed callbacks
-- and stale generations never modify the first client's accepted work.
local observer_receipts = {}
local observer = assert(app.client(config {on_event = function(event)
  if event.type == 1 then
    if event.input.type == 4 then observer_receipts[event.input.result.request_id] = event.input.result end
  end
end}))
connected(observer)
bound(observer)
local other = observer:get_state()
assert(other.transport.controller ~= other.transport.attachment)
assert(observer:request {operation = 'offer', request_id = '1', conversation = other.conversation, generation = other.transport.generation, text = 'MUST NOT RUN'})
drive(observer, function() return observer_receipts['1'] end)
assert(observer_receipts['1'].outcome == 1)
assert(observer:request {operation = 'offer', request_id = '18446744073709551615', conversation = other.conversation, generation = other.transport.generation, text = 'MAX ID MUST NOT RUN'})
drive(observer, function() return observer_receipts['18446744073709551615'] end)
assert(observer_receipts['18446744073709551615'].outcome == 1)
local pinned = assert(app.client(config {expected_instance = other.transport.instance}))
connected(pinned); detached(pinned)
local instance = other.transport.instance
local wrong_instance = string.char((instance:byte(1) + 1) % 256) .. instance:sub(2)
local wrong = assert(app.client(config {expected_instance = wrong_instance}))
local wrong_ok, wrong_reason = wrong:wait(1000)
while wrong_ok and wrong:get_state().transport.phase < 4 do
  wrong_ok, wrong_reason = wrong:wait(1000)
end
assert(not wrong_ok or wrong:get_state().transport.phase >= 4, wrong_reason)
assert(wrong:close())
local incompatible = assert(app.client(config {
  requirements = {{name = 'missing.claim', digest = string.rep('\0', 32)}}
}))
local compatible, conflict = incompatible:wait(1000)
while compatible and incompatible:get_state().transport.phase < app.WORKSPACE_DISCONNECTED do
  compatible, conflict = incompatible:wait(1000)
end
assert(not compatible and conflict:find('missing.claim', 1, true))
assert(incompatible:close())
assert(observer:takeover {request_id = '1', generation = other.transport.generation})
drive(observer, function() local t = observer:get_state().transport; return t.controller == t.attachment end)
-- Pump the old attachment's role update, then prove its old scope is rejected.
drive(c, function() return c:get_state().transport.controller ~= c:get_state().transport.attachment end)
assert(c:request {operation = 'offer', request_id = '5', conversation = s.conversation, generation = s.transport.generation, text = 'STALE MUST NOT RUN'})
drive(c, function() return receipts['5'] end)
assert(receipts['5'].outcome == 1)
assert(observer:cancel {request_id = '2', generation = observer:get_state().transport.generation})
detached(observer)
-- Detachment changes the authority generation. Observe that change before
-- selecting an explicit fresh scope; the adapter must never refresh it.
drive(c, function() return c:get_state().transport.controller == '0' end)
assert(c:takeover {request_id = '1', generation = c:get_state().transport.generation})
drive(c, function() local t = c:get_state().transport; return t.controller == t.attachment end)

-- Cursor-based catch-up preserves the same collection, with no model input.
local cursor = c:get_state().transcript
assert(cursor.position ~= '0' and cursor.sequence ~= '0')
assert(c:unsubscribe())
replay_done = false
assert(c:subscribe {request_id = '3', conversation = s.conversation, session = s.session,
  collection = cursor.collection, after_position = cursor.position, after_sequence = cursor.sequence})
drive(c, function() return replay_done end)

for _, channel in ipairs {'on_event', 'on_catalog', 'on_transcript', 'on_execution', 'on_prompt_history'} do
  local bad = assert(app.client(config {[channel] = function() error('LUA ' .. channel .. ' FAILED') end}))
  local failure
  local function advance()
    local ok, message = bad:wait(1000)
    if not ok then failure = assert(message):find('LUA ' .. channel .. ' FAILED', 1, true); assert(failure, message) end
  end
  if channel ~= 'on_event' and channel ~= 'on_catalog' then
    while bad:get_state().transport.phase ~= 2 do advance() end
    if channel == 'on_prompt_history' then
      assert(bad:prompt_history {request_id = '1', session = s.session})
    else assert(bad:subscribe {request_id = '1', session = s.session}) end
  end
  local deadline = os.time() + 10
  while not failure do assert(os.time() <= deadline); advance() end
  assert(bad:close())
  assert(c:get_state().transport.phase == 2)
end

for _, message in ipairs {{}, string.rep('X', 4096)} do
  local bad = assert(app.client(config {on_event = function() error(message) end}))
  local ok, reason = bad:wait(1000)
  while ok do ok, reason = bad:wait(1000) end
  assert(type(reason) == 'string' and #reason <= 1024)
  assert(bad:close())
end

-- Full replay supplies execution entries; reconnect does not resubmit prompts.
assert(c:unsubscribe())
replay_done = false
assert(c:subscribe {request_id = '4', session = s.session})
drive(c, function() return replay_done end)
assert(replayed_execution > 0)
local weak
do
  local cycle = {}
  cycle.client = assert(app.client(config {on_event = function() return cycle.client end}))
  weak = setmetatable({cycle.client}, {__mode = 'v'})
end
collectgarbage('collect'); collectgarbage('collect')
assert(weak[1] == nil, 'callback cycle retained an abandoned native attachment')
local resume = c:get_state()
detached(c)
rejected(c:pump()); rejected(c:prepare()); rejected(c:get_state()); rejected(c:get_view())
assert(c:close())
local returning, binding = nil, false
returning = assert(app.client(config {
  expected_instance = resume.transport.instance,
  on_event = function(event)
    if not binding and event.type == app.CLIENT_CONNECTION and
        event.state.transport.phase == app.WORKSPACE_CONNECTED then
      binding = true
      assert(returning:bind {request_id = '1', conversation = '0',
        producer = resume.producer, session = resume.session})
    end
  end,
}))
drive(returning, function() return returning:get_state().bound end)
assert(returning:get_state().producer == resume.producer)
assert(returning:get_state().session == resume.session)
detached(returning)
local stopper = assert(app.client(config()))
connected(stopper)
assert(stopper:shutdown {request_id = '1', generation = stopper:get_state().transport.generation})
drive(stopper, function() return stopper:get_state().transport.phase == 4 end)
assert(stopper:close())
print('LUA WORKSPACE CLIENT PASSED')
