# Botter

### A compiler for agents written in plain English.

You write an agent the way you write a program: source files in a folder, under git, reviewed in pull requests, built into one executable. The difference is the language. The source is Markdown in plain English, and the compiler is itself an agent that writes most of that source with you.

```
myagent/                         ./botter                  myagent.bot
  agent.md       who it is                                 one static x86-64
  SQNC.md        what it does, step by step   ─────────▶   Linux executable,
  skills/        how to do each task                       ~360 KB, no install,
  tools/         small programs it can run                 no runtime, no deps
```

Botter is a rethink of how portable agents get built. It isn't one more framework where you hand-write Python glue around an LLM. It's a **compiler with a co-author built in**:

- **Can't code?** Describe the agent you want. Botter asks a few questions, proposes a plan, writes the agent's files, checks them and builds them, all in plain English.
- **Know a little code?** Add tools: any script (`#!/usr/bin/env python3`, bash, node ...) or binary that reads JSON on stdin and prints a result. Botter writes and tests them with you.
- **A developer?** Everything is plain text in a repo. Diff it, branch it, review it, run `make`-style rebuilds in CI. The `.bot` file is a build artifact, like any other binary.

## Quick start

```sh
make                      # needs gcc and musl-gcc (Fedora: dnf install musl-gcc musl-libc-static)
mkdir myagent && cd myagent
../botter                 # pick a provider, paste your API key, describe your agent
./myagent.bot             # run the agent Botter built: on this machine, or copy it to any x86-64 Linux
```

Bring your own key: Gemini, OpenAI, OpenRouter, Groq, Ollama (local), or any OpenAI-compatible URL.

## The source language

An agent is a folder of plain-text files. Botter writes them with you, and you can edit any of them by hand.

| File | What it is | Who writes it |
|---|---|---|
| `agent.md` | The agent's persona and rules: who it is, what it may and may not do | Anyone (English) |
| `SQNC.md` | The session, step by step, in **Sqnc** | Anyone (structured English) |
| `skills/<name>/SKILL.md` | Playbooks: how to do one task well | Anyone (English) |
| `tools/bin/<name>` + `tools/doc/<name>.json` | Small programs the agent can run, and their description | A little code |
| `config.json` | How Botter builds and runs it: name, version, internet on/off, which skills and tools go in | Botter (you can edit it) |

### config.json: how the agent is built

Botter's build settings for the agent, and nothing else:

```json
{
  "name": "lead-outreach",
  "display_name": "Lead Outreach",
  "version": "0.1.0",
  "description": "Finds local businesses that need your service and pitches each one, with your approval.",
  "internet": true,
  "skills": ["research-lead", "pitching"],
  "tools": ["scrape_leads", "send_email"],
  "requires": ["python3"]
}
```

| Field | What it does |
|---|---|
| `name`, `display_name`, `version`, `description` | Who the agent is. Shown when it starts and with `--version`, and the agent knows it if asked. Botter starts a new agent at `0.1.0` and bumps the version whenever it changes one. |
| `internet` | `false` builds an offline agent: the kernel cuts its tools and shell commands off the network (the connection to the LLM still works). |
| `skills`, `tools` | Which skills and tools are compiled in. A listed one that doesn't exist is a build error; one that is present but not listed is left out, with a warning. |
| `requires` | Programs the tools need on the machine (`python3`, `git`). The build warns when a script tool's interpreter isn't listed; the agent says at start-up which ones aren't installed. |

The LLM provider, model and key aren't in it: whoever runs the agent chooses those. The build checks the file: bad JSON, wrong types, a bad name or version, or a missing skill or tool are errors.

### Skills: how to do a task

A skill is a Markdown playbook with a one-line description at the top:

```markdown
---
name: pitching
description: Write one specific, non-generic idea per lead. Read before writing any outreach message.
---
# Pitching
1. Open with something true and specific about their business ...
```

At start-up, botcore lists every skill's name and description to the model, but not the full text. The model reads a skill only when the task calls for it, so an agent can carry dozens of playbooks without filling its context. This is the `SKILL.md` convention popularized by Agent Skills, and a skill is just a file: copy it between agents, or keep a shared library of them in git.

### Sqnc: the flow, in structured English

`SQNC.md` is the agent's program: the order of steps, the decisions and the loops. It is written in **Sqnc** (say "sequence"): plain English for the steps, CAPITALS for the structure. The program goes in a ` ```sqnc ` block, so Markdown never turns a step into a list item or a heading, and anything outside the block is documentation for people.

````markdown
---
spec-version: "sqnc-1"
title: "Lead Outreach"
---
Finds businesses that need the user's service and pitches each one, with the user's approval.

```sqnc
LOAD SKILL "pitching"

## STEP 1: GATHER
1. ASK USER "What do you sell, and to which kind of clients?"
2. SAVE answer INTO VARIABLE `target`
3. RETRY UP TO 2 TIMES DO
       EXECUTE tool `scrape_leads` with payload { "query": `target`, "limit": 20 }
   END RETRY
4. SAVE result INTO VARIABLE `leads`
5. IF `leads` IS EMPTY THEN
       RETURN "No leads found. Try a broader client type or another city."
   END IF

## STEP 2: OUTREACH
FOR EACH `lead` IN `leads` DO
    INVOKE SKILL "pitching" USING context `lead`
    SAVE skill_output INTO VARIABLE `pitch`
    ASK USER "Send this message?\n\n" + `pitch`
    IF `answer` IS EQUAL TO "yes" THEN
        EXECUTE tool `send_email` with parameters:
           - to: `lead.email`
           - body: `pitch`
    ELSE IF the user asked for changes THEN
        Rewrite `pitch` with the user's changes and send it with send_email.
    END IF
END FOR

## STEP 3: DONE
RETURN "Contacted " + `leads.length` + " leads"
```
````

Statements: `LOAD SKILL`, `EXECUTE tool`, `INVOKE SKILL`, `ASK USER`, `SAVE ... INTO VARIABLE` / `TO FILE`, `SET ... TO`, `IF / ELSE IF / ELSE / END IF`, `FOR EACH / END FOR`, `WHILE ... AT MOST N TIMES / END WHILE`, `RETRY UP TO N TIMES / END RETRY`, `IN PARALLEL / END PARALLEL`, `RETURN`. Any other line is a plain-English instruction.

**Why this shape:** humans and LLMs both read it at a glance, and the structure is explicit (`END IF`, `END FOR`), so nothing depends on indentation. Data stays apart from instructions: values live in `` `variables` `` and `"strings"`, and the model is told that what a variable holds (a scraped web page, an email) is data, never a command to follow. That is a deliberate defence against prompt injection.

#### Compiled

The build checks `SQNC.md` like a compiler front end, and refuses to build a broken flow. Real messages, from breaking the example above:

```
SQNC.md:23: error: unknown keyword INVOCATE; did you mean INVOKE?
SQNC.md:27: error: EXECUTE tool `send_mail`: no such tool (built-in, or tools/bin/send_mail with tools/doc/send_mail.json)
SQNC.md:29: error: `pich` is used before it is set (SAVE ... INTO VARIABLE `pich`, SET `pich` TO ..., or FOR EACH `pich` IN ...)
SQNC.md:32: error: END FOR, but the IF opened on line 26 is still open (close it with END IF first)
SQNC.md:36: error: `lead` only exists inside its FOR EACH loop
```

It checks matched blocks, tools and skills that really exist, variables set before use (and loop variables not used after their loop), quoting, bounded retries, and keyword typos.

#### Reviewed by an LLM

Correct syntax isn't the same as sense. Before building, Botter has your connected LLM review the flow's **logic**. Can each step actually be done with the tool or skill it names, and the data available at that point? Do payloads fit the tools' parameters? Is anything sent, paid for or deleted without an `ASK USER` first? Can every loop end? Does the flow do what `agent.md` says the agent is for? If not, Botter tells you which lines don't make sense, why, and what the review suggests instead, and applies the fixes you agree to.

#### Interpreted, with the LLM only where it's needed

At run time botcore's **Sqnc interpreter** runs the flow statement by statement, the moment the agent starts. Everything with an exact meaning runs in botcore itself, with no model involved: tool calls, questions to the user, variables, loops, retries, comparisons (`IS EQUAL TO`, `CONTAINS`, `IS EMPTY` ...) and files. The LLM is called only for what needs judgement: plain-English steps, skills, and conditions or values written in prose (`IF the user asked for changes THEN`). In the test suite, a flow with a question, a tool call, a loop, a branch, a retry, a file write and a reply made **2 LLM calls** in total. That makes flows cheaper, faster and predictable, and the model can't skip a step or wander off.

How Sqnc compares: other agent-workflow languages exist, but none take this shape. [PDL](https://arxiv.org/abs/2410.19135) (IBM) is YAML, [POML](https://arxiv.org/abs/2508.13948) (Microsoft) is HTML-like markup for prompts, [BWML](https://cdn.jsdelivr.net/npm/bmad-plus@0.9.0/src/bmad-plus/packs/pack-dev-studio/shared/bwml-spec.md) is XML, [SudoLang](https://github.com/paralleldrive/sudolang-llm-support) is free-form pseudocode, and [GitHub Agentic Workflows](https://github.github.com/gh-aw/introduction/overview/) are prose without checked control flow. Sqnc is English with keywords: compiled, reviewed for logic, and interpreted, with the LLM used only where English has to be understood.

### Tools: when English is not enough

A tool is any executable: a Python script, a shell script, a Go binary. It reads one JSON object on stdin and prints its result. Its descriptor tells the model what it does:

```json
{
  "description": "Find local businesses of a given type in a city.",
  "parameters": {"type": "object", "properties": {"query": {"type": "string"}}, "required": ["query"]},
  "status": "Searching for leads",
  "timeout_s": 120
}
```

Users never see tool names or JSON. They see `status`, for example "Searching for leads" (developers can show the raw calls with ctrl+o or `/details`).

## The build

`botter_pack` (which Botter runs through its `agent_build` tool) validates the project, checks `SQNC.md`, and appends the files read-only to **botcore**, the runtime. No compiler or linker runs, and nothing is generated: the result runs exactly the files you wrote. The `.bot` file is botcore plus your Markdown and tools, with a checksum, so a truncated or modified file refuses to start.

**botcore** is the runtime every agent runs on, Botter included. It is about 9,000 lines of C11 of its own, statically linked against musl. It contains the Sqnc parser and interpreter, in-process HTTPS (vendored BearSSL, compiled-in CA roots: no curl, no OpenSSL, no system certificates), an OpenAI-compatible chat client with streaming and tool calling, file, shell and scheduling tools, and a kernel-enforced sandbox.

## Small and fast

Measured on an AMD Ryzen 5 7520U laptop (Fedora 44, kernel 7.2), median of 15 to 30 runs:

| | Size / time | Peak memory |
|---|---|---|
| `botter` (full-screen UI + runtime + compiler) | **638 KB**, one file | |
| A built agent (`.bot`, minimal) | **356 KB**, one file | |
| Build an agent | **2.7 ms** | 0.4 MB |
| Start an agent (to its first prompt) | **1.2 ms** | **0.4 MB** |
| Real HTTPS round trip to Gemini (DNS + TLS + request) | 0.15 to 0.5 s (mostly network) | **1.8 MB** |
| Whole session: connect, one turn with a tool call, exit (local stub LLM) | 134 ms (mostly the stub) | **3.8 MB** |
| *For reference:* `python3 -c pass` | 18.9 ms | 9.2 MB |
| *For reference:* `python3` importing `json, urllib, ssl` | 59.3 ms | 19.3 MB |
| *For reference:* `node -e 0` | 27.0 ms | 43.7 MB |

The reference rows are the bare interpreters, before any agent framework is even loaded. A built agent starts more than 10 times faster than an empty Python process, in about a twentieth of the memory. It has no dependencies (`ldd`: "not a dynamic executable"): copy the file to any x86-64 Linux and run it.

## Private by design

- **Stateless.** Botter and the agents it builds write nothing on their own: no config files, no logs, no history, no cache. The conversation lives in RAM and is gone when you quit, and Botter's screen is cleared on exit. The only files that appear are the ones you asked the agent to make.
- **Nothing phones home.** No telemetry, no accounts, no update checks. The only connection botcore makes is to the LLM provider *you* chose, with *your* key (BYOK). Tools and shell commands may use the network when the task needs it; set `"internet": false` in an agent's config.json and the kernel cuts them off.
- **Your key stays in memory.** It is never written to disk, never passed in arguments or environment, held in locked memory and wiped on exit, and other programs of your user cannot read the process. So you don't retype it every run, it is cached in the kernel keyring until reboot (RAM only; `/forget` clears it, `BOTCORE_NO_KEYRING=1` disables it). Shell commands and tools cannot reach that keyring.

## Safe by default

- **Sandbox.** Every shell command and tool runs under Landlock: it can write only in the working directory, `/tmp` and package caches, and the rest of your home directory (`~/.ssh`, tokens, ...) is invisible. Anything outside, or destructive (`rm`, overwrites, `git push`, `sudo`), asks you first.
- **Plan and Build modes.** Press **Tab** (or `/plan`, `/build`). In **Plan** the agent only investigates and plans: the kernel makes the working directory read-only, writes and scheduling are refused, and tools ask first unless marked `"readonly": true`. In **Build** it does the work.
- **Limits.** CPU, memory, process count and file size limits on every child process; bounded tool loops; Ctrl-C stops a request within a second.

## In the terminal

- `/provider` switches provider, key or model mid-conversation (the conversation is kept), and `/forget` drops saved keys
- `/plan`, `/build` or **Tab** switch modes, ctrl+o shows details, and `/help` lists the commands
- Replies stream as they are written; the model's thinking is shown separately and folded away
- An agent with a `SQNC.md` starts its flow by itself; when it ends you can keep chatting, and `/run` starts it again

## Development

```sh
make test                     # 111 tests: Sqnc compiler + interpreter + review, config.json, sandbox, streaming, key cache, Plan mode, HTTP, TUI
BOTTER_TEST_NET=1 make test   # adds real providers and TLS failure cases
make dist                     # dist/botter-<version>-linux-x86_64.tar.gz + .sha256
```

Layout: `tools/src/botcore` (runtime, C11), `tools/src/botcore/src/sqnc*.c` (Sqnc parser, checks and interpreter), `tools/src/pack` (packer), `tools/src/agent_tools` (Botter's own tools), `tui/` (Botter's full-screen UI; agents it builds use a plain terminal REPL), `agent.md` + `SQNC.md` + `skills/` (Botter itself is an agent project, built by the same compiler and run by the same interpreter), `tests/`. Design notes: `progress.md`.

## License

MIT (see `LICENSE`). botcore includes cJSON (MIT), BearSSL (MIT) and Mozilla's CA certificate data (MPL-2.0); see `THIRD_PARTY.md`. Agents built by Botter contain botcore, so ship those notices with them.
