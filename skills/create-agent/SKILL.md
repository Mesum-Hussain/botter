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
   Propose defaults if the user is vague. Do not ask what you can decide.

2. Check the working directory with fs_list. If agent.md or skills already exist, this is an existing project: read what is there and change it, do not start over. Never overwrite existing files without telling the user.

3. Write agent.md (read skills/write-agent-md-and-skills/SKILL.md first).

4. Write one SKILL.md per recurring task the agent should do well (same skill file explains how). Skip skills for trivial agents.

5. Only if the agent really needs capabilities that shell commands and file tools cannot give (a special parser, a calculation, a local program), add tools. Read skills/write-tool/SKILL.md. Prefer a script tool over a compiled one unless the user wants a native binary.

6. Call agent_manifest to generate manifest.md.

7. Build: read skills/build-agent/SKILL.md. Run agent_build with dry_run=true, fix all errors, then agent_build, then agent_inspect.

8. Tell the user: the file name, its size, how to run it (./<name>.bot), what it asks at startup (provider, API key, model), and what tools it contains. Mention any warnings from the build in plain words.

## Quality bar
- agent.md describes ONE clear role. A paragraph or two plus a few firm rules is better than a long essay.
- Every skill has a precise description line, because the agent decides what to read from it.
- Do not invent tools that you did not create. Do not mention capabilities the agent does not have (internet, memory between runs).
- Test script tools by hand before building (see write-tool).

## Iterating
After the first build, the user usually wants changes. Edit the files, rerun agent_manifest if skills or tools changed, then dry_run and build again. The new build overwrites the old .bot of the same name.
