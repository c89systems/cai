local application = require('cai.app')
local endpoint, root, runtime = assert(arg[1]), assert(arg[2]), assert(arg[3])
local function new_owner()
  return assert(application.open {no_config = true, home_directory = root .. '/private',
    runtime_directory = runtime})
end
local function arguments()
  return {'-p', 'custom', '--endpoint', endpoint, '-m', 'gpt-5.6-luna',
    '--no-mcp', '--no-image-generation', '--terminal', '--yolo', '--insecure',
    '--no-codex-agents-md'}
end
local function environment(value)
  local env = {CAI_OWNER_VALUE = value, CAI_API_KEY = 'fixture'}
  for _, name in ipairs {'HOME', 'XDG_CONFIG_HOME', 'XDG_STATE_HOME', 'XDG_CACHE_HOME',
      'XDG_DATA_HOME', 'PATH', 'NO_PROXY', 'no_proxy', 'TERM'} do env[name] = os.getenv(name) end
  return env
end
local function until_status(owner, predicate)
  local deadline = os.time() + 10
  while true do
    local status = assert(owner:get_status())
    if predicate(status) then return status end
    assert(os.time() <= deadline, 'owner status did not progress')
    assert(owner:wait_status(1000))
  end
end
local function ready(owner)
  local s = until_status(owner, function(s) return s.runtime_ready or not s.running end)
  assert(s.running and s.runtime_ready, s.error.message)
  return s
end
local function stopped(owner)
  return until_status(owner, function(s) return not s.running end)
end
local function rejected(value, message)
  assert(value == nil and type(message) == 'string', 'expected an actionable rejection')
end
local function host_unchanged()
  local file = assert(io.open('host-marker'))
  assert(file:read('*l') == 'HOST_CWD_MARKER'); file:close()
  assert(os.getenv('CAI_OWNER_VALUE') == nil)
end
if arg[4] == 'empty' then
  local explicit = new_owner()
  assert(explicit:start(arguments(), {directory = root .. '/third', environment = {}}))
  local s = until_status(explicit, function(s) return s.runtime_ready or not s.running end)
  assert(not (s.error.message or ''):find('CAI_REPLAY_TURNS', 1, true))
  if s.running then assert(explicit:control('stop')) end
  stopped(explicit); assert(type(explicit:join()) == 'number'); explicit:close()
  local captured = new_owner()
  -- No context arguments: native startup must capture the actual caller view.
  assert(captured:start(arguments()))
  local failed = stopped(captured)
  assert(failed.outcome == 'failed' and failed.error.message:find('CAI_REPLAY_TURNS', 1, true))
  assert(captured:join() ~= 0); captured:close()
  host_unchanged()
  print('EMPTY ENVIRONMENT PASSED')
  return
end

host_unchanged()
local check = new_owner()
assert(check:status_fd() >= 0)
local initial = assert(check:observe_status())
assert(not initial.running and not initial.has_server and initial.outcome == 'not_run')
local quiet, changed = check:wait_status(0)
assert(not changed and not quiet.running)
rejected(check:join())
rejected(check:start(false))
rejected(check:start({'ok\0bad'}))
rejected(check:start(arguments(), nil, {persist = true}))
for _, context in ipairs {false, {typo = true}, {directory = 'relative'}, {directory = '\0bad'},
  {environment = false}, {environment = {[''] = 'bad'}}, {environment = {['BAD=NAME'] = 'x'}},
  {environment = {NAME = false}}, {environment = {NAME = 'bad\0value'}}} do
  rejected(check:start(arguments(), context))
end
for _, lifetime in ipairs {false, {typo = true}, {persist = 1},
  {first_attachment_timeout_ms = -1}, {first_attachment_timeout_ms = 1.5},
  {first_attachment_timeout_ms = '18446744073709551616'}} do
  rejected(check:start_server(arguments(), nil, lifetime))
end
for _, timeout in ipairs {false, 1.5, -2, 2147483648} do rejected(check:wait_status(timeout)) end
assert(not check:get_status().running)
check:close(); check:close()
rejected(check:start(arguments())); rejected(check:status_fd()); rejected(check:observe_status())
rejected(check:wait_status(0)); rejected(check:join())
for _, field in ipairs {'on_event', 'on_replay'} do
  local observed = assert(application.open {no_config = true, [field] = function() error('WRONG THREAD') end})
  local ok, reason = observed:start_server(arguments())
  rejected(ok, reason); assert(reason:find('cai.app.client', 1, true))
  rejected(observed:start(arguments()))
  observed:close()
end

-- Two independently captured owners share the configured store. Neither
-- startup nor later tools may mutate Lua's cwd/environment or the other view.
local a, b = new_owner(), new_owner()
local a_args = arguments()
local a_context = {directory = root .. '/work', environment = environment('left=right\nsecond')}
local b_context = {directory = root .. '/second workspace', environment = environment('other owner')}
assert(a:start_server(a_args, a_context, {persist = true}))
-- The C API has copied both argv and environment before start returns.
a_args[6] = 'MUTATED AFTER START'
a_context.directory, a_context.environment.CAI_OWNER_VALUE = '/not-a-workspace', 'MUTATED AFTER START'
assert(b:start_server(arguments(), b_context, {persist = true}))
local a_ready, b_ready = ready(a), ready(b)
assert(a_ready.has_server and b_ready.has_server)
assert(a_ready.server.persist and a_ready.server.phase == application.ATTACHMENT_READY)
assert(a_ready.server_instance ~= b_ready.server_instance and #a_ready.server_instance == 16)
assert(type(a_ready.server.generation) == 'string' and type(a_ready.server.controller) == 'string')
assert(a_ready.effective.model == 'gpt-5.6-luna')
rejected(a:start(arguments())); rejected(a:run({'--version'}))
host_unchanged()

local output, tool_output, history_done = {}, {}, false
local client = assert(application.client {runtime_directory = runtime, workspace = root .. '/work',
  expected_instance = a_ready.server_instance,
  on_execution = function(e)
    if e.record then tool_output[#tool_output + 1] = e.record.data end
  end,
  on_transcript = function(e)
    if e.kind == application.TRANSCRIPT_DATA then output[#output + 1] = e.data end
    if e.kind == application.TRANSCRIPT_REPLAY_END then history_done = true end
  end,
})
local deadline = os.time() + 10
while client:get_state().transport.phase ~= application.WORKSPACE_CONNECTED do
  assert(os.time() <= deadline); assert(client:wait(1000))
end
assert(client:subscribe {request_id = '1', session = a_ready.session_id})
while not history_done do assert(client:wait(1000)) end
local id = assert(a:control('queue_prompt', 'FIRST OWNER PROMPT'))
local done = until_status(a, function(s) return s.metrics.has_last_turn and not s.metrics.turn_active and s.control_id >= id end)
assert(done.control_status == 0)
-- Transcript and status are independent channels; wait for the actual bytes.
deadline = os.time() + 10
while not table.concat(output):find('FIRST OWNER DONE', 1, true) do
  assert(os.time() <= deadline); assert(client:wait(1000))
end
assert(table.concat(tool_output):find('left=right', 1, true))
assert(a_ready.metrics.has_last_turn == false, 'copied status was mutated')
local second_id = assert(b:control('queue_prompt', 'SECOND OWNER PROMPT'))
until_status(b, function(s) return s.metrics.has_last_turn and not s.metrics.turn_active and s.control_id >= second_id end)
assert(client:detach())
while client:get_state().transport.phase ~= application.WORKSPACE_DISCONNECTED do assert(client:wait(1000)) end
assert(client:close())
until_status(a, function(s) return s.server.count == 0 end)
assert(a:get_status().running, 'persistent owner stopped on last detach')
host_unchanged()

assert(a:control('stop'))
local final = stopped(a)
assert(final.has_server and final.server.phase == application.ATTACHMENT_STOPPING and
  final.server.stop_reason == application.ATTACHMENT_STOP_REQUESTED)
assert(a:join() == 0)
assert(b:get_status().running)
-- Startup failure is asynchronous and joinable; the same userdata can retry.
assert(a:start_server(arguments(), b_context, {persist = true}))
local failed = stopped(a)
assert(failed.outcome == 'failed')
local retry, retry_error = a:start_server(arguments(), b_context)
rejected(retry, retry_error); assert(retry_error:find('join', 1, true))
assert(a:join() ~= 0)
assert(b:get_status().running)
local restored_context = {directory = root .. '/work', environment = environment('restored')}
assert(a:start_server(arguments(), restored_context, {persist = true}))
local restarted = ready(a)
assert(restarted.server_instance ~= a_ready.server_instance)
b:close() -- Explicit close performs STOP/join before releasing the native app.
rejected(b:get_status())
assert(a:get_status().running)

-- Garbage collection during an actual paused inference must cancel and join
-- before discarding its native pointers. Other owners keep their contexts.
local weak
local gc_client, gc_received
do
  local abandoned = new_owner()
  assert(abandoned:start_server(arguments(), {directory = root .. '/third', environment = environment('gc')}, {persist = true}))
  local gc_ready = ready(abandoned)
  gc_received = ''
  gc_client = assert(application.client {runtime_directory = runtime, workspace = root .. '/third',
    on_transcript = function(e)
      if e.kind == application.TRANSCRIPT_DATA then gc_received = gc_received .. e.data end
    end})
  deadline = os.time() + 10
  while gc_client:get_state().transport.phase ~= application.WORKSPACE_CONNECTED do
    assert(os.time() <= deadline); assert(gc_client:wait(1000))
  end
  assert(gc_client:subscribe {request_id = '1', session = gc_ready.session_id})
  assert(abandoned:control('queue_prompt', 'GC OWNER PROMPT'))
  while not gc_received:find('GC STREAM HELD', 1, true) do
    assert(os.time() <= deadline); assert(gc_client:wait(1000))
  end
  weak = setmetatable({abandoned}, {__mode = 'v'})
end
collectgarbage('collect'); collectgarbage('collect')
assert(weak[1] == nil and a:get_status().running)
while gc_client:get_state().transport.phase ~= application.WORKSPACE_DISCONNECTED do assert(gc_client:wait(1000)) end
assert(gc_client:close())
-- Immediate reacquisition proves that GC released endpoint/context ownership.
do
  local scoped <close> = new_owner()
  assert(scoped:start_server(arguments(), {directory = root .. '/third', environment = environment('scoped')}, {persist = true}))
  ready(scoped)
end
local reused = assert(application.open {no_config = true, home_directory = root .. '/private',
  runtime_directory = runtime, settings = {server = {persist = true}}})
assert(reused:start_server(arguments(), {directory = root .. '/third', environment = environment('reused')}, {persist = false, first_attachment_timeout_ms = '1'}))
local expired = stopped(reused)
assert(expired.has_server and not expired.server.persist and
  expired.server.stop_reason == application.ATTACHMENT_STARTUP_ABANDONED)
assert(reused:join() == 0); reused:close()

-- GC is valid immediately after start acceptance, before observing READY.
do
  local starting = new_owner()
  assert(starting:start_server(arguments(), {directory = root .. '/third', environment = environment('starting')}, {persist = true}))
end
collectgarbage('collect'); collectgarbage('collect')

-- A non-listening start supports controls/status without a terminal or client.
local local_owner = new_owner()
assert(local_owner:start(arguments(), {directory = root .. '/third', environment = environment('local')}))
assert(not ready(local_owner).has_server)
assert(local_owner:control('stop')); stopped(local_owner)
assert(local_owner:join() == 0); local_owner:close()
a:close()
host_unchanged()
print('LUA OWNERS PASSED')
