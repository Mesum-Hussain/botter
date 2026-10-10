agent_build packs a project folder into <folder name>.bot.

Embedded: agent.md, config.json, FLOW.md, skills/**, tools/doc/**, tools/bin/* (flat); when config.json lists "skills" / "tools", only those.
Not embedded: tools/src, artifacts, hidden files, symlinks, anything else.

Examples
  {"dry_run": true}                       validate the project in the working directory
  {}                                      build ./<working directory name>.bot
  {"dir": "helper", "output": "h.bot"}    build the sub-project ./helper to ./h.bot

The runtime comes from the running botter binary itself (everything in front of its own pack), so builds work offline on any machine and no compiler is needed. Output lines starting with "error" make the build fail; "warning" lines do not.
