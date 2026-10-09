<!-- OML v1: Write workflows in plain English with 4-space indentation. Use `set name = value` for variables; `if`/`elif`/`else` for branches (`elif`/`else` attach to the nearest unfinished `if`); `for item in collection:` for iteration; `while condition:` for repetition; `in parallel:` for concurrent tasks; `retry N times:` for bounded retries; `ask user` for input/approval; `save state to "name"` for checkpoints; and `return value` to finish. Conditions and actions may be plain English. Structural keywords are `set`, `if`, `elif`, `else`, `for`, `in`, `while`, `return`; other directives are recognized as phrases. Only use manifest-declared capabilities. The runtime must enforce permissions, approvals, iteration/resource limits, and safe failure; never execute arbitrary code or guess ambiguous instructions. -->

# Botter Session Flow

```python oml
read skills/create-agent/SKILL.md
set request = the user's first message

if the request is a question about botter or agents:
    answer it briefly
    return the answer

use fs_list on the working directory
if agent.md or skills already exist:
    set mode = "edit"
    read agent.md, manifest.md, flow.md and the skills that are there
else:
    set mode = "new"

ask user up to 3 short questions about purpose, users, limits and existing code, proposing defaults
if the user wants an offline agent:
    set offline = true
if the agent should wrap an existing repo:
    follow the wrap-existing-project skill
    if the repo is not on disk:
        use shell_exec to git clone it into artifacts/

show the user the plan: agent.md, skills, tools, flow.md
ask user to approve the plan
if the user does not approve:
    revise the plan with the user's changes and ask again, at most 3 times
    if the plan is still not approved:
        return "Stopped: plan not approved"

follow the write-agent-md-and-skills skill to write agent.md and the skills with fs_write
if the agent needs tools:
    for tool in the planned tools:
        follow the write-tool skill to write tools/bin and tools/doc for this tool
        retry 3 times:
            test the tool by hand with shell_exec and realistic stdin JSON
        if the tool still fails:
            ask user how to proceed with this tool
if offline is true:
    write agent.json with {"offline": true}
if the agent has more than one step:
    follow the write-flow skill to write flow.md

use the agent_manifest tool
retry 5 times:
    use the agent_build tool with dry_run true
    fix every error and every flow.md warning it reports
if errors remain:
    return the remaining errors and what the user can do

use the agent_build tool
use the agent_inspect tool on the built .bot
save state to "build"
return the file name, size, how to run it, what it asks at startup and what must be installed or running
```
