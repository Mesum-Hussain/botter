# BOTCORE + BOTTER: SPEC + STATUS (AI-consumed; terse by design)

Updated: 2026-10-08 | Phase: 5a done (botter agent built: ./botter). Next: owner tests botter with real LLM; Ph3d Landlock; Ph4 in-process HTTPS | Lang: C11 | Target: x86_64, latest Fedora (dev: F44, kernel 7.2, SELinux enforcing, Landlock+userns on, musl-gcc installed)
Legend: [D]=owner decided, [A]=assumption, [R]=recommendation (unapproved), [S]=spike needed, P0..P3=priority

## 1. PRODUCT
botcore [D] = CORE HARNESS: portable single-binary LLM-agent runtime. Agent (`<name>.bot`) = static botcore ELF + appended read-only pack (markdown + embedded tool executables). No compiler/linker anywhere; no packer inside the .bot.
botter [D] = meta-agent (itself an agent built on botcore) that authors/compiles other agents: `mkdir myagent && cd myagent && botter` -> scaffold, iterate on md/tool code, build -> `./<rootfolder>.bot`, runs on any x86_64 Linux. Keep botcore as small/optimised as possible [D].

## 2. PROJECT LAYOUT (agent project schema) [D]
```
<agent>/
  manifest.md          LLM-facing map (package.json-like: skills, tools, relations); botter generates, humans may edit
  agent.md             persona/behaviour -> system prompt
  skills/<n>/SKILL.md  task playbooks (frontmatter name/description)
  tools/               OPTIONAL
    bin/<name>         ELF x86-64 (any language) OR script starting `#!`
    doc/<name>.json    REQUIRED per bin: {"description", "parameters" (JSON Schema, opt), "timeout_s" (opt, default 60, max 600)}
    doc/<name>.md      optional prose docs (LLM reads via vfs_read)
    src/               source of COMPILED tools; NEVER embedded, NEVER deleted [D]
  artifacts/           build-time inputs, never embedded
```
Interpreted tools need no src/. Tool name = file name = [A-Za-z0-9_-]{1,64}, no clash with built-ins. Embedded: manifest.md, agent.md, skills/**, tools/doc/**, tools/bin/* (flat). Not embedded: tools/src, artifacts, hidden files, symlinks.
VFS (`src/vfs.{c,h}`): sorted `vfs_entry_t{path,data,len,kind}` + bsearch; `vfs_init` loads pack from the file's own trailer. Tools `vfs_list`/`vfs_read` exist only if a pack exists; hide/refuse executables; 32KB cap + offset, UTF-8-safe. System prompt = embedded agent.md (else built-in default) + cwd + offline/permission text + hint to read manifest.md.

## 3. BUILD MODEL + TOOL PROTOCOL [D, IMPLEMENTED]
No compiler shipped; users bring tools (binary = portable; script = interpreter must exist on target). Output `<rootfilename>.bot`. Temp writes at build time OK [D].
`botter_pack build <dir> <base> <out.bot>` | `check <dir> <base>` (dry run) | `list <file>`. base = static botcore OR a finished .bot (pack stripped: prefix up to footer blob_off). Build = copy base -> zero-pad to 4096 -> BLOB -> FOOTER -> chmod 755 (atomic rename). Errors: tool without descriptor, ELF not x86-64, bin neither ELF nor `#!`, bad tool name. Warnings: dynamic botcore/tool ELF, descriptor without bin, missing agent.md/manifest.md.
FORMAT (authoritative `src/blob.h`; botter_pack has own copy; bump magic digits on change): `[botcore ELF][pad][BLOB][FOOTER 32B]`. BLOB = "BOTBLOB1", u32 count, entry[32B: path_off,path_len,data_off,data_len,kind] sorted ascending, strings, data (NUL-terminated). FOOTER = blob_off u64, blob_len u64, crc32 u32, rsvd u32, "BOTPACK1". Loader: open `/proc/self/exe` (fallback AT_EXECFN), pread footer, validate bounds+crc32+index, mmap RO. No footer = plain botcore. Invalid footer = refuse start. Gap: truncated .bot (footer gone) looks like plain botcore.
TOOL PROTOCOL (`src/tool_ext.c`): args = one JSON object on stdin; stdout = result; exit 0 ok, else error (stderr, else stdout, + "[tool X exited with code N]"). Forked child, same sandbox as shell_exec (`tool_sandbox_apply`: setsid, cwd = ctx, rlimits CORE/FSIZE/CPU, empty netns L5 fail-closed), run via `memfd_create`+`fexecve` (ELF MFD_CLOEXEC; script memfd stays open for kernel's /proc/self/fd/N; interpreter checked on host before each run). argv[0] = tool name. Child also gets fd 3 = read-only `/proc/self/exe` of the agent + env `BOTCORE_RUNTIME_FD=3` (sandboxed tool cannot open /proc/<ppid>/exe: EACCES). Timeout kills process group; Ctrl-C kills; signal crash reported; output cap ~32KB; stderr 2KB. Startup dim warnings for skipped tools.
Tested F44 enforcing (unconfined_t), static-musl botcore, mock server only: ELF (musl static), python3 script, sh nonzero+stderr, SIGSEGV, timeout, missing interpreter, network blocked (ENETUNREACH), 100KB truncation, vfs_read refuses executables, dynamic-glibc botcore+pack ok.
Open risks: confined SELinux domains / `vm.memfd_noexec` / noexec /proc could block memfd exec (no fallback; temp-file exec would break zero-persistence, ask owner); containers w/o /proc; static ELF tools recommended.

## 4. UX + PROVIDER CONTRACT [D]
- Colours: RED = user (REPL `\033[31m>> \033[0m`) and errors (`Error:` bold red); BLUE = anything from agent: replies (`\033[34m>> \033[0m`), all connect-flow questions, `\033[1;34mConnected\033[0m`; LIGHT GREEN (`\033[92m`) = text the user types (set per-redraw in `ed_refresh`, reset right after); secret entry not echoed; DIM = status notes.
- Every run: choose provider -> paste key (no-echo) -> validate (cheap call) -> "Connected" -> loop. Nothing saved.
- Any OpenAI-compatible provider: `POST {base}/chat/completions`, Bearer, OpenAI tools format. Presets: Gemini (DEFAULT, `gemini-3.1-flash-lite`, base `https://generativelanguage.googleapis.com/v1beta/openai`; verified from docs 2026-10-08, verify model on compat endpoint via GET {base}/models), OpenAI, OpenRouter, Groq, Ollama local (http only for loopback), Custom URL [Q3: no more presets for now]. Non-Gemini require typing model.
- Key RAM-only: mlock + explicit_bzero on exit (TODO); never argv/env/disk. Context dir = cwd at launch.

## 5. CONSTRAINTS [D]
ZERO PERSISTENCE (no config/logs/cache/temp on host; history RAM-only; only files the user's task creates; memory feature deferred to v2; persona = agent.md). FULLY OFFLINE except HTTPS to the chosen LLM endpoint (shell, git, curl, ssh, package managers, all). Cron KEEP, RAM-only, session-lifetime.

## 6. SAFETY MODEL [D] (enforcement = [R])
Free inside ctx dir; ASK (tty y/N) before (a) any access outside ctx, (b) destructive/irreversible ops. LLM output untrusted. Layers:
- L1 path guard (hard): fs_* use realpath/longest-existing-prefix vs ctx; outside -> confirm. [DONE]
- L2 shell gate (soft, bypassable e.g. `python -c`): see §7 guard.c. [DONE]
- L3 Landlock [S, TODO]: per-call child ruleset RW ctx (+approved grants), RO system dirs; denial -> LLM asks -> user approves -> next call wider ruleset.
- L4 rlimits (CPU, FSIZE, CORE done; AS, NPROC TODO), timeout, killpg, output cap. [mostly DONE]
- L5 network (hard) [DONE]: child `unshare(CLONE_NEWUSER|CLONE_NEWNET)` + uid/gid map = empty netns; verified F44 (only `lo`, connect -> ENETUNREACH). Applies to shell AND ext tools. `tool_shell_probe()` at startup: if unavailable, dim warning + soft filter only; if probe passed but later unshare fails the command is refused. Fallback idea: seccomp denying socket(AF_INET/INET6/PACKET/NETLINK); Landlock net rules insufficient (TCP only). SELinux/userns on other policies [S].
shell_exec = fork + `/bin/sh -c`, own pipes, setsid, no popen.

## 7. STATE OF CODE (src/, `make` -> botcore; ~3400 LOC incl. tools; plain POSIX C11)
- `http.c`: curl subprocess; URL+Bearer+JSON body via curl stdin config (`-q -K -`), key never in argv (verified); body, status (`-w`), stderr; poll loop; SIGINT kills curl; rejects control chars; `--proto =http,https`; 32MB cap.
- `chat.c`: `chat_validate` = GET {base}/models (2xx ok; 404/405/501 tolerated; 400/401/403 = key rejected, Gemini returns 400); `chat_send` = POST {model,messages,tools}; RAM history; parses `choices[0].message.content` (string or parts), error text from `{error:{message}}` or `[{error}]`. `chat_set_system()` keeps system prompt as hist[0]. `trim_history()` drops oldest whole TURNS (user..final answer incl. tool traffic) while bytes > `CHAT_HIST_MAX_BYTES` (300KB, -D override) [Q5 resolved]; dim "(context full: N oldest messages dropped)"; caveat: trimmed turns not restored if that send fails. Tool loop: tool_calls -> store assistant msg VERBATIM (keeps Gemini `extra_content.google.thought_signature`, mandatory for Gemini 3) -> run via `tool_cb` -> append `{role:tool,tool_call_id,name,content}` (name per Gemini docs; make conditional if a provider 400s) -> repeat <= `CHAT_MAX_TOOL_ROUNDS`(25). Failure/Ctrl-C after tools ran keeps turn + assistant note; before any tool = full rollback.
- `term.c`: SIGINT (no SA_RESTART), no-echo key entry, `term_print_clean` strips ESC/control chars from LLM output. Line editor (tty, non-secret): raw mode ISIG kept, arrows, Home/End, Del, Ctrl-A/E/B/F/K/U/W/L/D, Ctrl-P/N + Up/Down history (RAM, 200, dedupe), UTF-8, single-row horizontal scroll, ESC parse 40ms poll.
- `main.c`: provider menu, URL rule (https; http only loopback), key/model prompts, validate, REPL. `/exit` `/quit`, Ctrl-D exits. Ctrl-C: quits during connect; aborts request mid-turn (rolled back); at prompt first press warns, second exits. `agent_prompt()` = pack agent.md else default. System prompt = persona + cwd + tool/offline/permission guidance + (pack) VFS hint, built on heap.
- `tools.c/h`: registry = static tables TOOLS_FS/SHELL/CRON/VFS (`tool_t{name,desc,schema,fn}`, `bool fn(const cJSON*, char *res, size_t cap)`) + dynamic ext tools; `tools_call` -> heap 32KB result, "ERROR: " prefix on false, sanitised UTF-8/control. In-process user-tool ABI CANCELLED. main `on_tool` prints dim `  [tool] name args`.
- `guard.c/h` (L1+L2): ctx = cwd; `guard_resolve` (longest existing prefix realpath + tail; ".." under non-existent prefix rejected; `inside` flag); `guard_confirm[_cat]` blue y/N (EOF/non-tty/Ctrl-C = deny; OVERWRITE adds `a`=always this session). `guard_shell_classify`: quote-aware tokenizer; DENY network cmds (curl wget ssh scp rsync nc socat ping dig pip npm cargo dnf apt docker ..., git clone/fetch/pull/push/remote/submodule); CONFIRM rm/rmdir/dd/mkfs/kill/shutdown/systemctl/mount/eval/source, sudo/doas/su, recursive chmod/chown, find -delete/-exec, mv/cp/ln/install/tee onto existing file, `>` onto existing, git clean/reset/restore/rebase/checkout --/branch -D/stash drop, `sh -c`, `$(..)`/backticks, heredocs, unparseable quoting, any word starting `/`,`~` or containing `../` resolving outside ctx (allow /dev/{null,zero,urandom,random,stdin,stdout,stderr}). Wrappers (sudo env nohup time nice timeout xargs exec command) skipped to reach the real command.
- `tool_fs.c`: `fs_list` (sorted, dirs `/`, symlinks `@`, sizes), `fs_read` (pread, 32KB + `offset`, UTF-8-safe, binary detect, regular files), `fs_write` (creates parents, O_NOFOLLOW, `append`; overwrite of non-empty existing asks y/N/a). Outside ctx -> confirm.
- `tool_shell.c`: `shell_exec` {command, timeout (default 30, max 600)}; stdin /dev/null, stdout+stderr merged, chdir ctx, rlimits CORE=0 FSIZE=1GiB CPU=timeout+5 (`tool_sandbox_apply`), 32KB cap, `[exit code N]`, Ctrl-C kills group; returns when shell exits (bg `&` jobs killed).
- `cron.c`: RAM-only, 16 slots, periodic/daily(local)/once; tools `cron_set cron_list cron_delete get_time`; fires ONLY while REPL idle (`term_set_idle(cron_due)` -> `TERM_TICK`, runs `[Scheduled task #N fired] <action>` as a turn; half-typed input stashed/restored); never on non-tty stdin; host TZ used.
- `vfs.c`, `tool_ext.c`, `blob.h`: see §2, §3.
Known limits: editor columns = codepoints (CJK/emoji misalign); no bracketed paste; multi-line paste in secret prompt may leave buffered lines; no SIGWINCH redraw until keystroke; non-streaming; still needs host `curl` (I1); no fs_delete/fs_mkdir (use gated shell_exec).
Tested: mock OpenAI server (/tmp/opencode/{mock2.py,t.sh,run3.py}, not in repo): multi-turn, UTF-8, escaping, ESC strip, HTTP 500, bad key, unreachable, pty (key not echoed, Ctrl-C, Ctrl-D, editor, cron). Real Gemini: owner tested plain chat OK (2026-10-08); tool calls vs real Gemini NOT yet tested. No ASAN/valgrind on dev box (MALLOC_CHECK_/PERTURB only).
Build: `make` (dev dynamic ./botcore), `make release` (static musl stripped ./botcore-static ~166KB, obj-musl/), `make clean`. Makefile globs src/*.c. Never mix obj/ and obj-musl/. No LTO for shipped botcore (Q4: own code ~45KB stripped, remaining size = libc + future TLS; choose musl/BearSSL).

## 8. BOTTER PROJECT [DONE Ph5a] (repo root is itself an agent project)
Files: agent.md, manifest.md, skills/{create-agent,project-layout,write-agent-md-and-skills,write-tool,build-agent}/SKILL.md, tools/bin/{agent_manifest,agent_build,agent_inspect} (ONE static musl ELF copied 3x, dispatches on argv[0]), tools/doc/*.{json,md}, tools/src/{botcore,pack,agent_tools}, artifacts/{botcore,botter_pack} (Makefile-built, not embedded), root Makefile. `make` -> `./botter` (~478KB: botcore 166KB + pack 310KB incl. 3x95KB tool ELFs; shrinkable if needed). `make tools|artifacts|clean`; needs gcc + musl-gcc.
Tools (`tools/src/agent_tools/agent_tools.c` #includes `../pack/botter_pack.c` with main renamed, + cJSON): `agent_manifest {dir?}` regenerates the block between `<!-- botter:generated:begin/end -->` in manifest.md (files; skills via SKILL.md frontmatter `description:`; tools from tools/bin + tools/doc/*.json), keeps hand text outside markers, idempotent. `agent_build {dir?, output?, dry_run?}`: dir default ".", output default `./<dir basename>.bot`; runtime = the RUNNING agent (fd 3, see §3), so no embedded botcore copy and no compiler; output inside cwd, refuses overwriting non-ELF; packer messages on stdout. `agent_inspect {file}` = `botter_pack list`. All paths symlink-safely resolved and must stay inside cwd (ext tools have no confirm channel).
Decisions: scaffolding by LLM via fs_write + skills (no scaffold tool: avoids overwrite y/N); botter output named by folder.
Tested (mock server): manifest create/idempotent, dry_run, build, inspect, built .bot runs + its tool + vfs_read, rebuild over existing .bot, outside-cwd/symlink/overwrite refusals, bad-tool errors, runtime prefix byte-identical to artifacts/botcore. NOT tested with real LLM driving the skills (owner: `mkdir x && cd x && ../botter`). After changing botcore, re-run `make` (artifacts/botcore -> botter).

## 9. ISSUES
P0 I1 HTTP via `curl` subprocess: breaks "any Linux" + key briefly in curl config. [R] in-process HTTPS (BearSSL static, minimal HTTP/1.1, CA bundle embedded in pack; no host cert reads).
P1 I4 Safety remaining: Landlock L3, RLIMIT_AS/NPROC, seccomp fallback if userns blocked (SELinux), mlock+explicit_bzero of key. Classifier bypasses covered only by L3/L5.
P2 I6 Concurrency: DROP libuv/epoll; if needed, single poll() loop + timerfd.
Resolved: I2 link model replaced by trailer append (no linker); I7 .gitignore.

## 10. PLAN / NEXT (priority order)
(0) Owner tries `./botter` with real LLM; tune agent.md/skills from failures. (a) Ph3d: Landlock + RLIMIT_AS/NPROC + mlock/explicit_bzero key. (b) Ph4: in-process HTTPS (BearSSL) + CA bundle in pack. (c) Tests in repo (only /tmp harness today). (d) Detect truncated .bot (footer missing). (e) Ph6: release, size pass. Streaming optional/deferred. Tool-calling vs real Gemini still to verify.
Done: Ph3a (chat, UI, system prompt, trimming, editor, tool loop, fs/shell/cron, guard), Ph3b VFS/pack, Ph3c trailer pack + ext tools + static musl, Ph5a botter.

## 11. AGENT RULES
- Rebuild: `make clean && make`; static: `make release`. Errors to stderr / user messages via main.c helpers. Never log keys. No new persistence; no host-file reads outside ctx (CA bundle/config from pack).
- Prefer editing over creating files. Update THIS file each session (status, issues, plan). Keep terse.
- GIT [D]: repo ~/Code/botter, public https://github.com/Mesum-Hussain/botter, branch main; botcore/pack are plain folders under tools/src (old botcore history: /tmp/opencode/botcore-git-backup.tar.gz, vanishes on reboot). Owner authorised COMMIT AND PUSH after each major update (finished feature/phase), not trivial edits. `gh` logged in (credential helper), global git user configured (Mesum Hussain <mdmesumhussain@gmail.com>). Check git status/diff first, stage only intended files, never commit secrets, no force-push/history rewrite unless asked. .gitignore: /botter, /botter.bot, /artifacts/, /tools/bin/, agent_tools binary, /graphify-out/, /pac.md.
- After code changes run `graphify update .` (graphify-out/ is generated, git-ignored).

## 12. LICENSING (audit 2026-10-08)
Repo: own C + cJSON (MIT, lib/cjson); LICENSE = MIT, Mesum Hussain + retained upstream copyright line. No GPL/AGPL source. glibc dynamic = OK; STATIC glibc = LGPL relink obligation -> release uses musl (MIT). libgcc runtime exception OK. Host curl and /bin/sh are separate processes (OK; curl goes away with I1). Future TLS lib must be permissive (BearSSL MIT; mbedTLS use Apache option); CA bundle (MPL-2.0) needs its notice shipped. Code uses only glibc/musl-common APIs.

## 13. OPEN / DECISIONS LOG
Resolved [D]: Q1 tools/src never deleted, only excluded from VFS/binary. Q2 manifest.md = package.json-like map, botter generates, humans may edit. Q3 no extra presets. Q4 no fixed size target, as small as possible. Q5 byte budget, drop oldest turns. Q6 botter may write/delete temp files at build time; no compiler shipped. Other [D]: tools embedded and run from memory; output `<rootfilename>.bot`; botter_pack lives outside botcore (now tools/src/pack); light-green user text; fs_write overwrite of non-empty file asks; any outside-ctx path word asks (noisy for `ls /usr`); tool results capped 32KB; cron fires only at idle prompt.
