---
name: write-agent-md-and-skills
description: How to write a good agent.md (persona = system prompt) and SKILL.md playbooks, with templates. Read before writing or editing either.
---

# Writing agent.md

agent.md becomes the agent's system prompt. The agent reads it on every request, so make it short and firm. Write to the agent in the second person, in plain text (the terminal does not render markdown).

Template:

```
You are <name>, <one-line role>.

Purpose
- <what you do for the user, 2-4 lines>

How you behave
- <tone, language, length of answers>
- <how you decide when to ask a question vs. act>

Rules
- <hard limits: things you must never do>
- <what to do when you do not know: say so, do not guess>

Working with files and tools
- Your skills and tools are listed in your system prompt and tool definitions.
- Do not tell the agent to read or follow FLOW.md: botcore runs it by itself and hands the agent one [Owl ...] step at a time. agent.md says who the agent is and its rules; FLOW.md is the order of steps.
- Before doing a task a skill covers, read that skill with vfs_read.
- <which tools to prefer for which job, if the agent has custom tools>
```

Guidance
- One role. If the user describes two jobs, make two agents or make one a skill.
- State what the agent cannot do: it keeps no memory between runs (unless it saves files in its working directory), and, for an offline agent only, it has no internet (or only through its "network" tools, named). Do not let it promise otherwise.
- Never put secrets in agent.md. It is embedded in the binary in readable form.
- Do not copy this template blindly; fill it with the user's real requirements and delete empty sections.

# Writing a skill

A skill is a playbook for one recurring task. File: skills/<name>/SKILL.md. The agent sees only the one-line `description:` from the frontmatter (the runtime lists it in the system prompt) and opens the skill when the task matches, so the description must say WHEN to use it.

Template:

```
---
name: <same as folder name>
description: <one line: what it does and when to use it>
---

# <Title>

Steps
1. <first concrete action, naming the exact tool and arguments if any>
2. <next>
3. <how to check the result>

Notes
- <pitfalls, formats, examples of good output>
```

Guidance
- Put procedures and examples in a skill, not in agent.md. agent.md stays small.
- Name the tools exactly as they appear (fs_read, shell_exec, or the custom tool name) and say what to pass.
- Keep each skill under about 60 lines. Split if longer.
- A skill may refer to other embedded files, e.g. tools/doc/<name>.md.
- Every SKILL.md starts with frontmatter (--- name: ... description: ... ---): without a description the agent cannot tell when to read it.
