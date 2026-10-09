---
name: create-agent
description: End-to-end workflow for creating a new agent from the user's idea to a built .bot file. Read this first for any "make me an agent" request.
---

# Creating an agent: workflow

Follow these steps in order. Keep the conversation short.

1. Understand the idea. In at most 3 questions find out:
   - what the agent is for and who talks to it
   - what it must do and what it must never do (limits, tone, language)
   - whether it needs tools beyond reading/writing files and running shell commands. Most agents do not.
   - network: agents are ONLINE by default (shell and tools can use the internet). Do not ask. Only if the user explicitly wants an offline agent, set "internet": false in config.json (see write-tool, section Network).
   - whether it should be built around existing code (a repo or scripts the user has). If yes, read skills/wrap-existing-project/SKILL.md.
   Propose defaults if the user is vague. Do not ask what you can decide.

2. Check the working directory with fs_list. If agent.md or skills already exist, this is an existing project: read what is there and change it, do not start over. Never overwrite existing files without telling the user.

3. Write agent.md (read skills/write-agent-md-and-skills/SKILL.md first).

4. Write one SKILL.md per recurring task the agent should do well (same skill file explains how). Skip skills for trivial agents.

5. Only if the agent really needs capabilities that shell commands and file tools cannot give (a special parser, a calculation, a local program, network access), add tools. Read skills/write-tool/SKILL.md. Prefer a script tool over a compiled one unless the user wants a native binary. The agent's own LLM does the thinking (analysing, writing, deciding, replying); tools only do I/O and computation. Do not write a tool that calls an LLM or orchestrates the whole job in a script: put the workflow in a skill and let the agent run it step by step.

6. If the agent has more than one step, write SQNC.md: the session flow in Sqnc, naming the agent's skills and tools exactly (read skills/write-flow/SKILL.md).

7. Build: read skills/build-agent/SKILL.md. Run agent_build with dry_run=true, fix all errors, then agent_build, then agent_inspect.

8. Tell the user: the file name, its size, how to run it (./<name>.bot), what it asks at startup (provider, API key, model, and, for an offline agent with a "network" tool, the internet permission question), what tools it contains, and what must be installed or running on the machine (interpreters, services). Mention any warnings from the build in plain words.

## Quality bar
- agent.md describes ONE clear role. A paragraph or two plus a few firm rules is better than a long essay.
- Every skill has a precise description line, because the agent decides what to read from it.
- Do not invent tools that you did not create. Do not mention capabilities the agent does not have (memory between runs; internet in an offline agent).
- No mocks, placeholders or hard-coded responses anywhere. If a real integration needs something you do not have (an API key, a running service), make the tool fail with a clear message saying what is missing.
- Always write config.json: name, display_name, version "0.1.0", a one-sentence description, the skills and tools to compile in, and the programs they need in "requires" (see project-layout, section config.json).
- Offline agent (only when asked): "internet": false in config.json; agent.md says it works offline. If a tool there has "network": true, agent.md names it and says the user is asked to allow it at startup.
- Test every tool by hand with realistic stdin JSON before building (see write-tool). A build only checks file formats, not that tools work.

## Iterating
After the first build, the user usually wants changes. Edit the files, bump "version" in config.json (PATCH for fixes, MINOR for new abilities, MAJOR for changed behaviour), then dry_run and build again. The new build overwrites the old .bot of the same name.
