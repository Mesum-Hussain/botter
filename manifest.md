# botter

botter builds other agents. The user runs it inside an agent project folder; it designs the project with the user, writes agent.md / skills / optional tools, and compiles the folder into <folder name>.bot. Start with skills/create-agent/SKILL.md. Text outside the generated block below is hand-editable and is kept.

<!-- botter:generated:begin (managed by the agent_manifest tool; edit outside these markers) -->
## Files
- agent.md: persona and behaviour (system prompt)

## Skills (read with vfs_read before the task they cover)
- skills/build-agent/SKILL.md: How to validate, build and verify the .bot file with agent_manifest, agent_build and agent_inspect, and how to read their messages. Read before building.
- skills/create-agent/SKILL.md: End-to-end workflow for creating a new agent from the user's idea to a built .bot file. Read this first for any "make me an agent" request.
- skills/project-layout/SKILL.md: Reference for the folder layout and file formats of an agent project, and what ends up inside the .bot. Read it when unsure where a file goes.
- skills/wrap-existing-project/SKILL.md: Building an agent around existing code (a cloned repo, scripts, a Docker or localhost service): turn it into tools and skills, handle prerequisites, licences, network. Read when the agent should use someone's repo.
- skills/write-agent-md-and-skills/SKILL.md: How to write a good agent.md (persona = system prompt) and SKILL.md playbooks, with templates. Read before writing or editing either.
- skills/write-tool/SKILL.md: How to add a custom tool to an agent: protocol, descriptor format, script and native examples, testing, portability. Read before creating anything under tools/.

## Tools (callable; arguments are described in the tool schema)
- agent_build [native]: Build an agent project into a single self-contained executable '<project folder name>.bot' (the botcore runtime plus the project's manifest.md, agent.md, skills/, tools/doc/ and tools/bin/ appended read-only). Validates tools first and r...  (docs: tools/doc/agent_build.md)
- agent_inspect [native]: List what is embedded in a built .bot file (files and tools with their sizes) and verify its checksum. Use it to confirm a build contains what you expect.  (docs: tools/doc/agent_inspect.md)
- agent_manifest [native]: Create or refresh manifest.md of an agent project: regenerates the block listing the project's skills (from skills/*/SKILL.md) and tools (from tools/bin + tools/doc/*.json) and keeps any hand-written text outside that block. Run it after...  (docs: tools/doc/agent_manifest.md)
<!-- botter:generated:end -->

## Notes
Workflow: create-agent -> (write-agent-md-and-skills, write-tool) -> agent_manifest -> agent_build dry_run -> agent_build -> agent_inspect.
- project-layout is the reference for where files go and what gets embedded.
- agent_build, agent_manifest and agent_inspect only accept paths inside the working directory.
- agent_build uses botter's own runtime as the base of the new agent, so it works offline and needs no compiler.
- Sources of botter's own tools: tools/src/agent_tools (not embedded). Runtime: tools/src/botcore. Packer: tools/src/pack.
