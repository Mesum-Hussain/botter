---
name: project-layout
description: Reference for the folder layout and file formats of an agent project, and what ends up inside the .bot. Read it when unsure where a file goes.
---

# Agent project layout

The project root is the folder the user started botter in. The built file is named after that folder: <root folder name>.bot

```
<agent>/
  agent.md             persona and behaviour = the agent's system prompt
  config.json          how to build and run the agent: name, version, internet, skills, tools ... (always write it; see "config.json" below)
  SQNC.md              OPTIONAL (recommended for multi-step agents): the session flow in Sqnc: one ```sqnc block (frontmatter + UPPERCASE statements, no Markdown), checked by the build, run by botcore at start-up (see write-flow)
  skills/<name>/SKILL.md   task playbooks: how and when to use tools
  tools/                OPTIONAL
    bin/<name>          the runnable tool: native x86-64 ELF, or a script starting with #!
    doc/<name>.json     REQUIRED for every bin: descriptor (description, parameters, timeout_s, network)
    doc/<name>.md       optional prose docs: examples, caveats, read via vfs_read
    src/<name>/         source of COMPILED tools. Never embedded, never deleted.
  artifacts/            build-time inputs (not embedded), e.g. a cloned repo you wrap into tools
```

## What is embedded in the .bot (read-only)
agent.md, config.json, SQNC.md, skills/** , tools/doc/** , tools/bin/* (flat, no sub-folders); when config.json lists "skills" / "tools", only those

## What is NOT embedded
tools/src, artifacts, hidden files (names starting with a dot), symlinks, everything else (README, .git, notes, test data).

## Rules
- agent.md is the system prompt. If missing, the agent runs with a generic default prompt (build warns).
- There is no manifest: at start-up the runtime lists the skills (path + frontmatter description) and tool docs in the system prompt; tool schemas come from tools/doc/*.json. If SQNC.md is present, botcore runs it when the agent starts (the Sqnc interpreter) and gives the agent one step at a time. So skill and tool descriptions must be accurate and short.
- Interpreted tools need no tools/src: the script in tools/bin is the source.
- Tool name = file name in tools/bin = [A-Za-z0-9_-]{1,64}. It must not clash with a built-in tool.
- Skill folder names: lowercase letters, digits and dashes, e.g. summarize-logs.
- Average users have no tools folder at all. That is fine: an agent with only agent.md and skills is a complete agent.

## Every agent also has these built-in tools (no work needed)
fs_list, fs_read, fs_write (files in the working directory), shell_exec (sandboxed; online unless the agent is offline), cron_set / cron_list / cron_delete (schedules that live only while the agent runs), get_time, and vfs_list / vfs_read (the embedded read-only files above, only present if a pack exists).

## Runtime facts the agent author should know
- The agent connects to an OpenAI-compatible LLM endpoint chosen by the user at every start (provider, API key, model). Nothing is saved to disk: no config, no logs, no memory between runs.
- The agent is online by default: its shell and tools can reach the internet and localhost services.
- With config.json "internet": false it is offline except for the LLM connection: its shell and tools cannot reach the network (not even localhost services), except tools whose descriptor has "network": true, and only after the user allows it at startup.
- For a completely offline agent, the user can also pick "Ollama (local)" as provider, so not even the LLM call leaves the machine.
- It is free inside its working directory and asks the user before touching anything outside it or doing something destructive. The kernel enforces this for shell commands and tools (Landlock): writes only in the working directory, /tmp and package caches; the rest of $HOME is hidden unless the user approves a command that names the path.

## config.json
How Botter builds and runs the agent. Every agent has one, and it holds only this (nothing about who made it or which LLM it uses: the person running the agent chooses the provider). The build checks it: bad JSON, wrong types, a bad name or version, or a listed skill or tool that does not exist are errors.

{
  "name": "lead-outreach",
  "display_name": "Lead Outreach",
  "version": "0.1.0",
  "description": "Finds local businesses that need the user's service and pitches each one, with approval.",
  "internet": true,
  "skills": ["research-lead", "pitching"],
  "tools": ["scrape_leads", "send_email"],
  "requires": ["python3"]
}

- name: lowercase letters, digits, - _ . (1-64); the project folder's name. display_name: the spelling people see (shown at start-up).
- version: MAJOR.MINOR.PATCH. A new agent starts at "0.1.0". When you change an existing agent, bump it before building: PATCH for fixes, MINOR for new abilities, MAJOR when it behaves differently for its users.
- description: one sentence, what it does and for whom (shown at start-up; the agent knows it).
- internet: false only when the user asks for an offline agent (see write-tool, section Network). Default true.
- skills, tools: the skills (folder names) and tools (tools/bin names) compiled into the agent. List every one you wrote; a skill or tool left off the list stays in the folder but is not built in.
- requires: every program the agent's tools or shell steps need on the machine (python3, node, git, ffmpeg ...). The build warns when a script tool's interpreter is missing from it; the agent says at start-up which ones are not installed.
- builder: true only for agents that build agents (Botter itself).
