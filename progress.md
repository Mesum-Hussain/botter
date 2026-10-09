# BOTCORE + BOTTER: SPEC + STATUS (AI-consumed; terse by design)

Updated: 2026-10-09 | Phase: Ph3d done (Landlock, rlimits, key hardening); next Ph4 in-process HTTPS | Lang: C11 | Target: x86_64 Linux, latest Fedora (dev: F44, kernel 7.2, SELinux enforcing, Landlock+userns on, musl-gcc)
Legend: [D]=owner decided, [R]=recommendation (unapproved), [S]=spike needed, P0..P3=priority

## 1. PRODUCT [D]
- botcore = portable single-binary LLM-agent runtime. Agent `<name>.bot` = static botcore ELF + appended read-only pack (markdown + tool executables). No compiler/linker/packer inside a .bot. Keep botcore small.
- Botter (display name; binary `botter`) = meta-agent built on botcore: `mkdir x && cd x && botter` -> designs project with user, writes files, builds `./<folder>.bot`.

## 2. AGENT PROJECT LAYOUT [D]
```
manifest.md   LLM map (generated block between <!-- botter:generated:begin/end -->, hand text kept)
agent.md      persona -> system prompt; tells LLM: read manifest.md, then flow.md
flow.md       OPTIONAL session flow in OML v1 (see §3b)
agent.json    OPTIONAL {"offline": true}; absent = online
skills/<n>/SKILL.md   playbooks (frontmatter name/description)
tools/bin/<name>      ELF x86-64 or #! script;  tools/doc/<name>.json REQUIRED {description, parameters, timeout_s<=600, network}
tools/doc/<name>.md   optional;  tools/src/ compiled-tool source, never embedded/deleted
artifacts/            build inputs (cloned repos), never embedded
```
Embedded: manifest.md agent.md agent.json flow.md skills/** tools/doc/** tools/bin/* (flat). Tool name [A-Za-z0-9_-]{1,64}, no built-in clash. Output named after folder.

## 3. BUILD + TOOLS [D, DONE]
- `botter_pack build|check|list`. Format (authoritative src/blob.h, packer has a copy): `[botcore ELF][pad 4096][BLOB "BOTBLOB1" sorted entries][FOOTER 32B crc32 "BOTPACK1"]`. Loader reads /proc/self/exe footer, mmap RO; bad footer = refuse start.
- Errors: tool w/o descriptor, non-x86-64 ELF, bin neither ELF nor #!, bad name. Warnings: dynamic ELF, descriptor w/o bin, missing agent.md/manifest.md, OML lint.
- Tool protocol: args = one JSON object on stdin (argv empty); stdout result; exit!=0 = error (stderr). Run from memfd+fexecve in sandbox child (setsid, cwd=ctx, rlimits, killpg on timeout/Ctrl-C, 32KB cap). fd 3 = agent's own exe (BOTCORE_RUNTIME_FD) so agent_build can use it as runtime.
### 3a. Network mode [D 2026-10-09]
- ONLINE by default (botter + built agents): shell_exec/tools keep host network, network cmds allowed, `git push` asks, no consent prompt/probe.
- `agent.json {"offline": true}` (invalid JSON => offline): children in empty netns (userns) else seccomp (blocks AF_INET/INET6/PACKET); guard denies curl/wget/pip/docker/git clone...; per-tool `"network": true` exception after startup y/N consent. Botter writes agent.json ONLY on explicit user request.
- LLM calls are made by the botcore parent: never sandboxed, always work.
### 3b. flow.md / OML v1 [D 2026-10-09]
- Owner-defined pseudo code (stand-in for LangGraph/n8n). File = verbatim `<!-- OML v1: ... -->` header (text in skills/write-flow) + `# Name` + ONE ```python oml fenced block (fence keeps indentation; "python" = highlighting on GitHub/editors; lint + TUI key on the word "oml", TUI has its own OML highlighter).
- Followed by the LLM (no engine); botcore appends FLOW_HINT to system prompt when flow.md exists; guards still enforce approvals/limits.
- botter_pack oml_lint (warnings, fenced body only): missing header/fence/unclosed fence, tabs, indent%4, ':' opener needs +4 body, unexpected indent, if/elif/else/for/while need ':', elif/else need unfinished if, `for x in y:`.

## 4. UX + PROVIDERS [D]
- Built agents: plain REPL (red user `>>`, blue agent, light-green typed text, dim status). Every run: provider -> key (no echo) -> model -> validate GET /models -> "Connected". Nothing saved.
- OpenAI-compatible `POST {base}/chat/completions`. Presets: Gemini default (`gemini-3.1-flash-lite`), OpenAI, OpenRouter, Groq, Ollama (http loopback only), Custom URL. No more presets.
- Botter TUI (§7) only for botter; generated agents keep the REPL.

## 5. CONSTRAINTS [D]
ZERO PERSISTENCE by runtime (no config/logs/cache; history RAM-only; only files the task creates). Cron RAM-only, fires at idle prompt. Memory feature deferred.

## 6. SAFETY MODEL
Free inside ctx (cwd at launch); ask y/N before outside-ctx access or destructive ops. LLM output untrusted.
- L1 path guard (fs_*: realpath vs ctx) DONE. L2 shell classifier (soft) DONE: confirms rm/dd/kill/sudo/`sh -c`/`$()`/overwrites/outside paths/destructive git/git push; offline adds network-cmd deny.
- L3 Landlock [DONE Ph3d] for every child (shell_exec + tools), all agents: RW ctx, /tmp, /var/tmp, /dev, ~/.cache, ~/.npm; RO+exec system dirs (/usr /etc /proc /sys /run /var /opt ...), $PATH dirs + their prefix if .../bin (not $HOME or /), ~/.gitconfig. Rest of $HOME hidden. Approved shell command naming outside paths -> those paths (or deepest existing parent) granted RW for that run only; approved sudo -> no Landlock that run. ABI probed (1..5 rights); unavailable -> dim note; apply failure -> refuse (fail closed). Needs no_new_privs (setuid can't elevate except approved-sudo runs). Runtime fd 3 opened before sandbox.
- L4 rlimits: CORE 0, FSIZE 1GiB, CPU timeout+5, NPROC = user's procs at startup + 1024 (fork-bomb brake), DATA = physical RAM (chosen over AS: AS breaks V8/Go/WASM reservations). L5 network isolation: offline agents only (§3a).
- Key: RAM-only, never argv/env/disk; mlock'd + explicit_bzero on free; process PR_SET_DUMPABLE 0 (no ptrace/proc mem by same user; netns child re-enables before unshare, else uid_map is root-owned).

## 7. CODE MAP
botcore `tools/src/botcore/src` (~3.5K LOC): http.c (curl subprocess, key via stdin config), chat.c (validate, tool loop <=25 rounds, assistant msgs verbatim for Gemini thought_signature, turn-based trim at 300KB, reasoning events), term.c (line editor, SIGINT), main.c (connect flow, network mode, system prompt), tools.c (registry), guard.c (L1/L2, offline flag), tool_fs.c, tool_shell.c (shell_exec + tool_sandbox_apply), tool_ext.c, cron.c, vfs.c, front.c (BOTCORE_FRONTEND=1 JSON-record protocol for the TUI).
Botter (repo root is an agent project): agent.md, flow.md, manifest.md, skills/{create-agent,project-layout,write-agent-md-and-skills,write-tool,write-flow,wrap-existing-project,build-agent}, tools agent_manifest/agent_build/agent_inspect (one static ELF `tools/src/agent_tools`, includes packer). Skills enforce: no mocks, agent's own LLM does reasoning (no call_llm tools), stdin-JSON tools with declared parameters, test tools by hand.
TUI `tui/` (main.c, lay.c, tui.h, logo.h): `make` bundles botter-tui + artifacts/botter.bot (trailer "BOTTUI01"), runs agent from memfd with BOTCORE_FRONTEND=1; non-tty/TERM=dumb/BOTTER_TUI=0 -> plain REPL. Look: terminal's own bg (nothing painted); sage palette 5C7057/89A482/ACC5A6/D1EDD3 light/dark set via OSC 11 (else COLORFGBG, else dark; BOTTER_THEME overrides); Jupyter-like gutter `In [n]:`/`Out[n]:`, user msg = tinted bordered cell, chat box = next In cell; markdown/code/tables, collapsible thinking/tools (ctrl+o), scrollback, mouse wheel, paste, history. Logo 11x12 muted head (no crown/limbs/body).
Build: `make` -> ./botter; `make tools|artifacts|tui|clean`; botcore `make release` = static musl ~170KB. Needs gcc + musl-gcc. After botcore changes re-run `make`.
Tested with stub OpenAI servers (scratchpad only): chat, tools, network modes, OML lint, TUI captures via tmux, Landlock (ctx/tmp write OK, $HOME write/list denied, ~/.local/bin tools run, approved path granted once, agent_build OK, offline netns+Landlock). NOT tested: real-LLM tool calling end to end, real Gemini thoughts.

## 8. ISSUES
P0 I1 HTTP via host `curl` (not "any Linux"; key briefly in curl config) -> Ph4 in-process HTTPS (BearSSL, CA bundle in pack).
P1 I4 Classifier bypassable (`python -c`); hard layers cover fs (L3) and offline net (L5). An approved new-file path grants its existing parent (often $HOME) RW for that run.
P2 memfd exec may be blocked by confined SELinux / vm.memfd_noexec (no fallback). Truncated .bot looks like plain botcore. Editor width = codepoints (CJK). Non-streaming.

## 9. PLAN / NEXT
(0) Owner: build the hackathon agents with ./botter + real LLM (problem 2: marketing agent on google-maps-scraper-kit, must work e2e from 2 inputs, model-agnostic, no mocks, opt-out + send caps); tune skills from failures. First attempt (marketbot) failed: argv tools, mock LLM tool -> fixed in skills.
(a) Ph4 [NEXT]: in-process HTTPS (BearSSL static, minimal HTTP/1.1, CA bundle in pack) -> drop host curl.
(b) Tests in repo. (c) Detect truncated .bot. (d) Ph6 release/size pass. Deferred: websearch tool, streaming, memory.
Done: Ph3d Landlock + rlimits + key hardening (2026-10-09); Ph5a-e botter, TUI, network mode, flow.md.

## 10. AGENT RULES
- Prefer editing over new files; update THIS file each session, keep terse. Never log keys; no new persistence.
- GIT [D]: ~/Code/botter -> github.com/Mesum-Hussain/botter, branch main. Commit AND push after each finished feature/phase. Check status/diff, stage intended files only, no secrets, no force-push. Ignored: /botter, /artifacts/, /tools/bin/, agent_tools binary, tui binaries, /graphify-out/, /pac.md.
- After code changes run `graphify update .`.

## 11. LICENSING
Own C + cJSON (MIT); LICENSE MIT. Release static = musl (avoid static glibc LGPL). Future TLS must be permissive (BearSSL MIT); CA bundle (MPL-2.0) notice must ship.
