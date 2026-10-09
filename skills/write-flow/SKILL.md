---
name: write-flow
description: How to write flow.md, the agent's session flow in OML (plain-English pseudo code), with the exact header to copy and examples. Read before writing or editing any flow.md.
---

# flow.md: the session flow in OML

flow.md tells the agent WHEN to use which skill and tool, step by step, for a whole session. manifest.md says WHAT exists; skills say HOW to do one task; flow.md is the order and the decisions between them. It is a small stand-in for LangGraph / n8n graphs: no engine runs it, the agent's LLM reads and follows it, and botcore's guards (approvals, sandbox, timeouts) still apply.

Every agent with more than one step gets a flow.md in the project root. A one-shot agent (answers a question, done) does not need one.

## File format
1. First line: this exact comment, copied verbatim (the build warns if it is missing):

<!-- OML v1: Write workflows in plain English with 4-space indentation. Use `set name = value` for variables; `if`/`elif`/`else` for branches (`elif`/`else` attach to the nearest unfinished `if`); `for item in collection:` for iteration; `while condition:` for repetition; `in parallel:` for concurrent tasks; `retry N times:` for bounded retries; `ask user` for input/approval; `save state to "name"` for checkpoints; and `return value` to finish. Conditions and actions may be plain English. Structural keywords are `set`, `if`, `elif`, `else`, `for`, `in`, `while`, `return`; other directives are recognized as phrases. Only use manifest-declared capabilities. The runtime must enforce permissions, approvals, iteration/resource limits, and safe failure; never execute arbitrary code or guess ambiguous instructions. -->

2. Then a markdown heading with the flow's name (`# Lead Outreach Flow`).
3. Then the logic inside ONE fenced code block that starts with ```oml and ends with ```. The fence keeps it pseudo code: without it, markdown merges lines into paragraphs and eats the indentation. Nothing goes after the closing fence.
4. Inside the fence: one statement per line, 4-space indentation, no tabs. A line ending in ':' opens a block; its body is indented exactly 4 more spaces.

## Statements
- `set name = value`                       variable (value may be plain English: `set leads = the scraped leads`)
- `if cond:` / `elif cond:` / `else:`       branches; elif/else attach to the nearest unfinished if at the same indentation
- `for item in collection:`                 iteration
- `while cond:`                             repetition; always give a bound in the condition ("while fewer than 20 leads and under 3 attempts")
- `in parallel:`                            the body's steps are independent and may run in any order or concurrently
- `retry N times:`                          bounded retry of the body; say what happens after the last failure
- `ask user ...`                            input or approval; nothing outward-facing (sending, paying, deleting) without it unless the user said so
- `save state to "name"`                    checkpoint: write progress to a file in the working directory (e.g. state/name.json) so a later run can resume
- `return value`                            finish the flow (or a branch) with this result
- Anything else is an action in plain English. Name the skill or tool it uses: `use the scrape_leads tool with the target and city`, `follow the pitching skill`.

## Rules
- Only use capabilities in manifest.md: built-in tools, this agent's tools, its skills. Never invent a tool.
- Name tools and skills exactly as in manifest.md.
- Keep each line one clear step. If a step needs a paragraph, it belongs in a skill; the flow names the skill.
- Every loop and retry is bounded. Every failure path ends in `ask user` or `return` with a clear message.
- Approvals: put `ask user` before anything irreversible or outward-facing.
- After writing it, run agent_build with dry_run=true: flow.md structure problems show as warnings "flow.md line N: ...". Fix them all.

## Example (the whole file)

````markdown
<!-- OML v1: ... (the full header above, verbatim) -->

# Lead Outreach Flow

```oml
ask user what service they offer and who their clients are
set service = the user's service
set clients = the user's client type

retry 2 times:
    use the scrape_leads tool for clients near the user's city
if no leads were found:
    return "No leads found; try a broader client type or city"

save state to "leads"

for lead in the scraped leads:
    in parallel:
        follow the research-lead skill for this lead
        check the outreach log for an earlier contact with this lead
    if the lead was contacted before or opted out:
        skip this lead
    else:
        follow the pitching skill to write one specific idea for this lead
        if this is the first send of the session:
            ask user to approve the message
        use the send_email tool with the message and the opt-out line
        save state to "outreach"

return a summary of leads found, messages sent and replies waiting
```
````
