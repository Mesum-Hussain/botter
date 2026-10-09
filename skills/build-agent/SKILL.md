---
name: build-agent
description: How to validate, build and verify the .bot file with agent_manifest, agent_build and agent_inspect, and how to read their messages. Read before building.
---

# Building

Steps
1. agent_manifest {}              refresh manifest.md (skills and tools list). Do this whenever skills/tools changed.
2. agent_build {"dry_run": true}  validate only. Fix every "error" line, then repeat until it says "check passed".
3. agent_build {}                 build ./<project folder name>.bot in the working directory (or pass "output").
4. agent_inspect {"file": "<name>.bot"}   list the embedded files and tools; confirm everything expected is there.
5. Tell the user to run it: ./<name>.bot

Arguments: "dir" is the project folder relative to the working directory (default ".", the working directory itself). All paths must stay inside the working directory.

What the build does: copies the botcore runtime (the same engine botter runs on) and appends the project files to it. No compiler or linker runs. The result is one static x86-64 executable. Rebuilding replaces the old .bot.

## Warnings you may see
- "flow.md: put the OML code inside a ```python oml fenced block": wrap the logic in ```python oml ... ``` (see write-flow).
- "flow.md line N: ...": OML structure problem (indentation, a missing ':', elif/else without if, an empty block). Fix it; the agent follows a broken flow badly.
- "flow.md does not start with the <!-- OML v1 ... --> description comment": copy the header from skills/write-flow/SKILL.md.

## Errors you may see
- "tools/bin/X has no descriptor tools/doc/X.json": add the descriptor (see write-tool).
- "tools/bin/X is neither an ELF executable nor a script starting with #!": the file is not a program. Add a #! line or put in a real binary.
- "ELF binary but not for x86-64": wrong architecture; rebuild the tool for x86-64.
- "tool names must be 1-64 chars of [A-Za-z0-9_-]": rename the file (and its .json/.md).
- "refusing to overwrite ... not an executable": the output path points at an existing non-.bot file; choose another output name.
- "outside the working directory": use a path under the working directory.

## Warnings (build still succeeds)
- "dynamically linked": that tool only runs where its shared libraries exist. Relink statically for a portable agent.
- "no agent.md": the agent will use a generic prompt. Write agent.md.
- "no manifest.md": run agent_manifest.
- "descriptor without matching executable": a tools/doc/X.json exists but tools/bin/X does not; that tool will not exist.

## After building
- Running the .bot asks for a provider, API key and model each time and saves nothing.
- Script tools need their interpreter on the machine that runs the agent; native static tools need nothing.
- The .bot can be copied to any x86-64 Linux machine.
