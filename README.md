# Botter

Botter is an agent that builds other agents. You describe the agent you want; Botter designs it with you, writes its files and compiles them into **one self-contained Linux executable** (`<name>.bot`) that runs on any x86-64 Linux with no installation.

Every agent, Botter included, runs on **botcore**: a small static runtime (~315 KB) that talks to any OpenAI-compatible LLM (Gemini, OpenAI, OpenRouter, Groq, Ollama, or a custom URL), with built-in HTTPS, file and shell tools, scheduling, and a kernel-enforced sandbox.

## Quick start

```sh
make                      # needs gcc and musl-gcc (Fedora: dnf install musl-gcc musl-libc-static)
mkdir myagent && cd myagent
../botter                 # pick a provider, paste an API key, describe your agent
./myagent.bot             # run the agent Botter built
```

The provider, key and model are asked once per boot: after a successful connect they are kept in the kernel keyring (RAM only, never on disk, gone at reboot), so later runs reconnect by themselves. `/provider` switches provider, key or model mid-conversation, `/forget` drops the saved keys, `BOTCORE_NO_KEYRING=1` turns the cache off. Nothing else is saved between runs.

### Plan and Build modes

Like OpenCode: **Build** (default) lets the agent change things; **Plan** is read-only, so the agent investigates and writes a plan. Switch with **Tab** in Botter's UI, or `/plan` and `/build` in any agent. In Plan mode `fs_write` and `cron_set` refuse, shell commands and tools run with the working directory read-only (kernel-enforced by Landlock), `sudo` is refused, and agent tools ask first unless their descriptor says `"readonly": true`.

## What an agent is made of

```
myagent/
  agent.md          persona and rules (the system prompt)
  flow.md           optional: the session flow in OML, a plain-English pseudo code
  agent.json        optional settings: {"offline": true} for an offline agent
  skills/<n>/SKILL.md   playbooks, with a frontmatter description of when to use them
  tools/bin/<name>  optional tools: any x86-64 ELF or #! script (JSON in on stdin, result on stdout)
  tools/doc/<name>.json  each tool's description and JSON Schema parameters
```

`agent_build` appends these files read-only to the botcore runtime. At start-up the runtime lists the agent's skills and tools to the model by itself; there is no manifest to keep in sync.

### flow.md (OML)

```oml
ask user what service they offer and who their clients are
retry 2 times:
    use the scrape_leads tool for those clients
if no leads were found:
    return "No leads found"
for lead in the scraped leads:
    follow the pitching skill for this lead
    ask user to approve the message
return a summary
```

OML has `set`, `if`/`elif`/`else`, `for … in`, `while`, `in parallel`, `retry N times`, `ask user`, `save state to`, `return`; everything else is plain English. The agent's model follows it step by step; the build checks its structure.

## Safety

- **Network**: agents are online by default. With `agent.json` `{"offline": true}` shell commands and tools are cut off from the network by the kernel (the LLM connection still works); single tools can be allowed back with `"network": true`, after the user agrees at start-up.
- **Files**: shell commands and tools run in a Landlock sandbox: they may write only in the working directory, `/tmp` and package caches, and cannot see the rest of your home directory. Anything outside, or destructive, needs your approval first.
- **Keys** stay in memory (locked, wiped on exit); the process cannot be inspected by other programs of your user. The per-boot key cache lives in the kernel keyring, readable only by processes holding your login session's keyring; shell commands and tools are cut off from the keyring (seccomp), so a prompt-injected command cannot read it.

## Development

```sh
make test                 # 63 tests: stub LLM, sandbox, OML lint, HTTP client, Ctrl-C
BOTTER_TEST_NET=1 make test   # adds real providers and TLS failure cases
make dist                 # dist/botter-<version>-linux-x86_64.tar.gz + .sha256
```

Layout: `tools/src/botcore` (runtime, C11), `tools/src/pack` (packer), `tools/src/agent_tools` (Botter's own tools), `tui/` (Botter's full-screen UI; agents it builds use a plain terminal REPL), `skills/` + `agent.md` + `flow.md` (Botter itself), `tests/`. Design notes and status: `progress.md`.

## License

MIT (see `LICENSE`). botcore includes cJSON (MIT), BearSSL (MIT) and Mozilla's CA certificate data (MPL-2.0); see `THIRD_PARTY.md`. Agents built by Botter contain botcore, so ship those notices with them.
