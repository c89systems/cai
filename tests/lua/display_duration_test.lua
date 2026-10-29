local cai = require("cai")
local out = {}
local display = assert(cai.tool_display({color=true}, function(bytes) out[#out+1]=bytes end))
assert(display:set_palette("\27[38;5;141m", "\27[38;5;219m"))
assert(not display:set_palette("\27[1;31m", ""))
assert(not display:set_palette("", "\27[48;5;3m"))
assert(not pcall(display.set_palette, display, "\27[31m\0tail", ""))
for _, case in ipairs{{0,"0ms"},{999,"999ms"},{1000,"1s"},{63000,"1m3s"},{5025000,"1h23m45s"}} do
  out={}
  assert(display:event{type=cai.AGENT_EVENT_TERMINAL_COMMAND_STARTED,
    tool_call_id="live",runtime_session_id="s",data="command"})
  assert(display:event{type=cai.AGENT_EVENT_TERMINAL_COMMAND_COMPLETED,
    tool_call_id="live",runtime_session_id="s",terminal_duration_ms=case[1],
    terminal_has_exit_code=true,terminal_exit_code=0})
  assert(table.concat(out):find("\27[0m\27[38;5;141mRan \27[0;1m\27[38;5;219mcommand\27[0m\27[38;5;141m ("..case[2]..")\27[0m\n",1,true))
  out={}
  assert(display:replay_begin({type=cai.AGENT_EVENT_TOOL_CALL_COMPLETED,
    tool_call_id="saved",runtime_session_id="s",terminal_duration_ms=case[1]},"Ran","saved command",true))
  assert(table.concat(out):find("\27[0m\27[38;5;141mRan \27[0;1m\27[38;5;219msaved command\27[0m\27[38;5;141m ("..case[2]..")\27[0m\n",1,true))
end
assert(display:set_color(false))
out={}
assert(display:replay_begin({type=cai.AGENT_EVENT_TOOL_CALL_COMPLETED,
  tool_call_id="saved",runtime_session_id="s",terminal_duration_ms=9},"Ran","saved command",true))
assert(table.concat(out):find("* Ran saved command (9ms)\n",1,true))
assert(not table.concat(out):find("\27",1,true))
assert(display:set_color(true))
assert(display:set_palette("", ""))
out={}
assert(display:summary({type=cai.AGENT_EVENT_TOOL_CALL_COMPLETED}, "Read", "default color", true))
assert(table.concat(out):find("\27[0mRead \27[0;1mdefault color\27[0m\n",1,true))
assert(not pcall(display.event,display,{type=cai.AGENT_EVENT_TOOL_CALL_COMPLETED,terminal_duration_ms=-1}))
out={}
assert(display:event{type=cai.AGENT_EVENT_REASONING_TOOL_RESULT,
  tool_call_id="reason",runtime_session_id="s",
  data="requested high (high): medium -> high"})
assert(table.concat(out):find("requested high (high): medium -> high",1,true))
display:close()
