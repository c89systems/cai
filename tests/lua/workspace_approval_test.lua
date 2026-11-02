local cai = require("cai")
local root = assert(arg[1]) .. "/lua-workspace-approval"
local function quote(s) return "'" .. s:gsub("'", "'\\''") .. "'" end
assert(os.execute("mkdir -p -- " .. quote(root .. "/child")))
for _, dir in ipairs({root, root .. "/child"}) do
  local file = assert(io.open(dir .. "/AGENTS.md", "w"))
  file:write("Lua workspace instruction fixture\n")
  file:close()
end
local client = assert(cai.open {api_key="fixture", logger_disabled=true,
  base_url="http://127.0.0.1:1/v1"})
for mode = 0, 2 do
  local config = {workspace_directory=root .. "/child",
    agent_config_directory=root .. "/missing-global", disable_terminal=true,
    disable_default_session_store=true, disable_chrome=true,
    disable_web_search=true, disable_review_subagent=true, disable_skills=true,
    logger_disabled=true, insecure=mode==1,
    workspace_approval_directory=mode==2 and root or nil}
  local runtime, err = client:new_smith_runtime(config)
  assert(runtime, err and err.message)
  local status = assert(runtime:instruction_status())
  assert(status.repository_agents_md_count == (mode==0 and 0 or 1))
  runtime:close()
end
local agent = assert(client:new_smith_agent {workspace_directory=root .. "/child",
  agent_config_directory=root .. "/missing-global", disable_terminal=1,
  workspace_instructions_root=root})
assert(assert(agent:instruction_status()).repository_agents_md_count == 1)
agent:close()
client:close()
assert(os.remove(root .. "/child/AGENTS.md"))
assert(os.remove(root .. "/AGENTS.md"))
assert(os.execute("rmdir -- " .. quote(root .. "/child") .. " " .. quote(root)))
