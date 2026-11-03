local cai = require("cai")
local client = assert(cai.open {api_key="fixture", base_url="http://127.0.0.1:1/v1",
  logger_disabled=true, http_2_disabled=1})
local config = {workspace_directory="/tmp", disable_default_session_store=true,
  disable_terminal=true, disable_chrome=true, disable_web_search=true}
local runtime = assert(client:new_smith_runtime(config))
local a, b = assert(runtime:pending_input()), assert(runtime:pending_input())
assert(a:state().count == 0)
local function accepted(queue, request)
  local result = assert(queue:request(request))
  assert(result.outcome == "accepted", result.reason)
  return result
end
local offered = accepted(a, {operation="offer", text="held input", enabled=true})
assert(a:entry(1).held)
assert(not a:claim(1), "held input cannot be claimed")
local edit = accepted(a, {operation="begin_edit", item_id=offered.item_id,
  item_revision=offered.item_revision})
assert(edit.edit_token ~= "0" and b:entry(1).edit_token == "0")
local rejected = assert(b:request {operation="withdraw", item_id=edit.item_id,
  item_revision=edit.item_revision})
assert(rejected.outcome == "rejected")
local changed = accepted(a, {operation="end_edit", item_id=edit.item_id,
  item_revision=edit.item_revision, edit_token=edit.edit_token, text="edited input"})
assert(a:entry(1).text == "edited input" and a:entry(1).held)
accepted(a, {operation="approve", item_id=changed.item_id, item_revision=changed.item_revision})
assert(a:set_ready(1, true))
assert(b:entry(1).ready)
assert(a:claim(1).text == "edited input")
assert(a:state().count == 0 and b:state().count == 0)
local other = assert(client:new_smith_runtime(config))
local c = assert(other:pending_input())
accepted(a, {operation="offer", text="only in the first conversation"})
assert(c:state().count == 0)
assert(not a:request {operation="invalid"})
assert(not a:request {operation="offer", text="bad\0text"})
assert(not a:request {operation="offer", text="bad identity", request_id="18446744073709551616"})
-- A queue userdata pins allocation, not a closed runtime's journal authority.
runtime:close()
assert(not a:state() and not b:request {operation="offer", text="after close"})
a:close(); a:close(); b:close()
assert(not a:state())
c:close(); other:close(); client:close()
collectgarbage("collect")
