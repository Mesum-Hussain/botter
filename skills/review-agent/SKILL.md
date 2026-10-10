---
name: review-agent
description: Before building, read every file of the agent project and list each requirement of the request and plan that the files do not implement yet. Reply with a JSON list of gaps.
---

# Reviewing an agent project against its requirements

The build only checks formats. This review checks that the files do what the user asked. Do not fix anything in this step: only find the gaps.

1. Read every project file: agent.md, config.json, FLOW.md, each skills/*/SKILL.md, each tools/bin/* and tools/doc/*.json. Read them; do not rely on memory of what you wrote.
2. List the requirements: every input, behaviour, rule, limit, state file and notification in the request, the user's answers and the plan (e.g. "replies are answered in the same thread", "opted-out leads are never contacted", "daily cap of 10 counted from data/sent.json", "TEST_RECIPIENT redirects every lead email").
3. For each requirement, find the exact place that implements it: a tool action in the code, a FLOW.md step, a skill rule. Missing or only mentioned in prose without code or a step = a gap.
4. Also a gap:
   - a tool action, parameter or output named in its descriptor or in the plan but not implemented in its code (or the reverse);
   - a tool that would crash on real data (an e-mail with several parts, an empty file, a missing data/ folder, a key missing from .env) or prints more than ~32KB (trim the fields, save the full data to a file);
   - FLOW.md that never calls a tool or skill the plan needs, or uses a user's answer for the wrong purpose;
   - placeholder or hard-coded content where real content is needed.

Reply with only a JSON list, one object per gap, most important first:
[{"file": "tools/bin/gmail", "gap": "no 'send' action: first emails cannot be sent", "fix": "add action send: to, subject, body; apply TEST_RECIPIENT"}]
Reply [] if every requirement is implemented.
