---
name: write-tool
description: How to add a custom tool to an agent: protocol, descriptor format, script and native examples, testing, portability. Read before creating anything under tools/.
---

# Custom tools

A tool is an external program embedded in the .bot and started by the agent when the LLM calls it. There is no compiler in botter: you write the source, the user (or you, with shell_exec, if the machine has the toolchain) produces the executable.

Only add a tool when file tools and shell commands are not enough.

## Protocol
- The call arguments arrive on STDIN as one JSON object, e.g. {"city":"Oslo"}.
- Print the result to STDOUT (plain text or JSON). Output beyond ~32KB is cut.
- Exit code 0 = success. Non-zero = error: whatever you print to STDERR (else stdout) is shown to the LLM as the error. Write helpful messages.
- It runs with the working directory = the agent's working directory, inside a sandbox: NO network, limited file size and CPU. Default timeout 60 s (descriptor timeout_s, max 600).
- Read stdin fully before answering. Do not prompt the user: stdin is not a terminal.

## Files for a tool called <name>
1. tools/bin/<name>        the executable (see kinds below). Must be executable-looking: ELF or starts with #!
2. tools/doc/<name>.json   REQUIRED descriptor:
```
{
  "description": "What it does and when the LLM should call it (one or two sentences).",
  "parameters": {
    "type": "object",
    "properties": {
      "path": {"type": "string", "description": "File to analyse, relative to the working directory"},
      "limit": {"type": "integer", "description": "Max lines. Default 100"}
    },
    "required": ["path"]
  },
  "timeout_s": 60
}
```
   "parameters" is a JSON Schema object (optional: no arguments). "timeout_s" optional.
3. tools/doc/<name>.md     optional prose: examples, caveats. The agent reads it with vfs_read.
4. tools/src/<name>/       source of COMPILED tools only (never embedded, never deleted).

Tool name: letters, digits, underscore, dash, 1-64 chars. Not the name of a built-in tool.

## Kind 1: script (simplest)
The file starts with a #! line. The interpreter must exist on the machine that RUNS the agent (the agent tells the LLM if it is missing). Prefer #!/bin/sh or #!/usr/bin/env python3 (widely present).

```
#!/usr/bin/env python3
import json, sys
args = json.load(sys.stdin)
n = int(args.get("n", 1))
print(sum(range(1, n + 1)))
```
The file does not need the executable bit (the agent runs it from memory). Test it by hand:
  shell_exec: echo '{"n": 10}' | python3 tools/bin/<name>

## Kind 2: native executable
Any language that produces an x86-64 Linux ELF. Static linking makes it run on any Linux; a dynamically linked tool only runs where its shared libraries exist (the build warns).
Put the source in tools/src/<name>/ and the binary in tools/bin/<name>. With a C compiler on the machine (check first with shell_exec: command -v musl-gcc gcc):
  musl-gcc -Os -static -o tools/bin/<name> tools/src/<name>/<name>.c      (preferred, fully static)
  gcc -Os -static -o tools/bin/<name> tools/src/<name>/<name>.c           (needs static glibc)
If no compiler exists, do not pretend: write the tool as a script, or give the user the source and the compile command and let them place the binary in tools/bin.
Test: echo '{"n": 10}' | tools/bin/<name>

## Checklist
- Descriptor valid JSON? Every bin has a .json, every .json has a bin.
- Tested with realistic input AND with bad input (it should exit non-zero with a clear message).
- Runs offline, writes only inside the working directory.
- Mentioned in the relevant skill, so the agent knows when to use it.
- Then run agent_manifest and the build (build-agent skill).
