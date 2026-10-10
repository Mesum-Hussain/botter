---
name: wrap-existing-project
description: Building an agent around existing code (a cloned repo, scripts, a Docker or localhost service): turn it into tools and skills, handle prerequisites, licences, network. Read when the agent should use someone's repo.
---

# Wrapping an existing project

Put the code on disk inside the project first (clone it yourself, or ask the user for a local copy):
  git clone <url> artifacts/<repo-name>
artifacts/ is never embedded. You read it, then copy or adapt what the agent needs into tools/ and skills/.

## 1. Study it (read, do not guess)
- README, setup docs, licence, the scripts/CLI entry points, example inputs, config templates (.env.example, docker-compose.yml).
- Files written for AI assistants are gold: CLAUDE.md, AGENTS.md, .claude/skills/*/SKILL.md, .claude/commands/*.md. They contain the operating manual (parameters, limits, failure signs, legal guardrails). Turn them into this agent's skills, removing anything specific to another tool (slash commands, "use the Bash tool", settings files).
- Write down: what it does, inputs, outputs, what must be installed or running (python3, docker, a server on localhost:PORT), which steps use the network, how long a run takes.

## 2. Decide the tools
- One tool = one file in tools/bin. It must follow the protocol (JSON object on stdin, result on stdout, exit 0 / non-zero). Existing CLIs usually take argv, so write a small adapter, or adapt the script itself into a single self-contained file (stdlib only: the agent cannot pip install).
- Prefer a few task-shaped tools (e.g. scrape_start, scrape_status, scrape_download) over one tool with dozens of flags. Expose only the parameters the agent needs; keep sane defaults from the repo.
- Big results: write them to a file in the working directory (CSV/JSON) and return a short summary plus the path and a few preview rows. Tool output is cut at ~32KB.
- Wrap the repo's real entry point with its real required arguments (read its usage line). No mock fallbacks.
- The code runs sandboxed: it cannot read or write $HOME outside the working directory (only /tmp and ~/.cache are writable). Point any config, credentials file or output path the repo uses at the working directory (e.g. keep .env there).
- Understanding, ranking and writing (e.g. reading a lead and inventing a pitch) is the agent's own LLM's job, guided by a skill. Do not hide it in a script.
- Only if the agent is offline (config.json "internet": false) does a step that talks to a server, even on localhost, need "network": true in that tool's descriptor (read write-tool, section Network).
- A tool returns what the agent needs next (the leads, the sent message id), not an intermediate handle: a scrape tool that only returns a job id is useless unless status and download tools exist too. Jobs up to 10 minutes: one tool that starts, waits (timeout_s up to 600) and returns the result. Longer: split into start / status / download and let the agent poll (or use cron_set to check back).
- Inputs the service needs but the user will not know (coordinates for a city, an id) are the tool's job: e.g. the scraper needs lat/lon, so the tool takes a city and geocodes it (the repo's own scrape.py shows how).

## 3. Services the code depends on (Docker containers, local servers)
- Prefer that the user starts them (docker compose up -d). An online agent may run docker itself through shell_exec if the user agrees; an offline agent cannot.
- Every networked tool first checks the service is reachable and, if not, fails with the exact fix, e.g. "Scraper not reachable at http://localhost:8080. Start it with: docker compose up -d (in a folder containing docker-compose.yml)".
- If a config file is needed (docker-compose.yml), put its content in a skill or tools/doc/<tool>.md so the agent can write it into the working directory with fs_write and tell the user the command to run.

## 4. Licence and credit
- Check the licence before copying code into tools/bin. MIT/BSD/Apache: keep the copyright and licence notice at the top of the copied file and mention the source in tools/doc/<tool>.md. GPL/AGPL or no licence: do not copy; call the user's local install instead, or ask the user.
- Respect the repo's usage rules (rate limits, terms of service, privacy law). Put them in agent.md as firm rules.

## 5. Write agent.md and skills
- agent.md: the agent's job in the user's words (the problem it solves, not the repo's feature list), prerequisites it must check, its hard rules.
- Skills: the operating manual (from the repo's own docs), plus the end-to-end workflow for the user's problem, e.g. "find leads -> clean -> qualify -> write outreach list".

## 6. Test before building
- Start the service the way the user will, then run each tool by hand: echo '{...}' | python3 tools/bin/<name>
- Test the failure path too (service stopped): the message must tell the user what to do.
- Then agent_build dry_run, agent_build, agent_inspect (build-agent skill). Tell the user what must be installed/running (and, for an offline agent with "network" tools, that it asks for internet permission at startup).
