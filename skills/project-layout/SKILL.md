---
name: project-layout
description: Reference for the folder layout and file formats of an agent project, and what ends up inside the .bot. Read it when unsure where a file goes.
---

# Agent project layout

The project root is the folder the user started botter in. The built file is named after that folder: <root folder name>.bot

```
<agent>/
  manifest.md          map of the project for the LLM (generated block + free text)
  agent.md             persona and behaviour = the agent's system prompt
  skills/<name>/SKILL.md   task playbooks: how and when to use tools
  tools/                OPTIONAL
    bin/<name>          the runnable tool: native x86-64 ELF, or a script starting with #!
    doc/<name>.json     REQUIRED for every bin: descriptor (description, parameters, timeout_s, network)
    doc/<name>.md       optional prose docs: examples, caveats, read via vfs_read
    src/<name>/         source of COMPILED tools. Never embedded, never deleted.
  artifacts/            build-time inputs (not embedded), e.g. a cloned repo you wrap into tools
```

## What is embedded in the .bot (read-only)
manifest.md, agent.md, skills/** , tools/doc/** , tools/bin/* (flat, no sub-folders)

## What is NOT embedded
tools/src, artifacts, hidden files (names starting with a dot), symlinks, everything else (README, .git, notes, test data).

## Rules
- agent.md is the system prompt. If missing, the agent runs with a generic default prompt (build warns).
- The agent reads manifest.md first at run time, then the skills it needs, through the vfs_list and vfs_read tools. So the manifest and skill descriptions must be accurate and short.
- Interpreted tools need no tools/src: the script in tools/bin is the source.
- Tool name = file name in tools/bin = [A-Za-z0-9_-]{1,64}. It must not clash with a built-in tool.
- Skill folder names: lowercase letters, digits and dashes, e.g. summarize-logs.
- Average users have no tools folder at all. That is fine: an agent with only agent.md and skills is a complete agent.

## Every agent also has these built-in tools (no work needed)
fs_list, fs_read, fs_write (files in the working directory), shell_exec (sandboxed, offline), cron_set / cron_list / cron_delete (schedules that live only while the agent runs), get_time, and vfs_list / vfs_read (the embedded read-only files above, only present if a pack exists).

## Runtime facts the agent author should know
- The agent connects to an OpenAI-compatible LLM endpoint chosen by the user at every start (provider, API key, model). Nothing is saved to disk: no config, no logs, no memory between runs.
- The agent is offline except for the LLM connection. Its shell and tools cannot reach the network (not even localhost services), except tools whose descriptor has "network": true, and only after the user allows it at startup.
- For a completely offline agent, the user can also pick "Ollama (local)" as provider, so not even the LLM call leaves the machine.
- It is free inside its working directory and asks the user before touching anything outside it or doing something destructive.
