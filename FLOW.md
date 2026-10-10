```owl
---
spec-version: "owl-1"
title: "Botter Session"
description: "Design an agent with the user, write its files, check them and build them into one .bot file."
---

LOAD SKILL "create-agent"
LOAD SKILL "project-layout"
LOAD SKILL "write-agent-md-and-skills"
LOAD SKILL "write-tool"
LOAD SKILL "write-flow"
LOAD SKILL "wrap-existing-project"
LOAD SKILL "build-agent"

STEP 1: UNDERSTAND
1. ASK USER "What agent should I build? Tell me what it should do, for whom, and anything it must never do. (Or ask me a question.)"
2. SAVE answer INTO VARIABLE `request`
3. IF `request` is a question about Botter or agents rather than a request to build or change one THEN
       Answer the question in `request` briefly.
       RETURN "Ask me anything else, or describe an agent to build."
   END IF
4. EXECUTE tool `fs_list` with payload { "path": "." }
5. SAVE result INTO VARIABLE `existing`
6. IF `existing` CONTAINS "config.json" OR `existing` CONTAINS "agent.md" THEN
       Read config.json, agent.md, FLOW.md and the skills in this folder, and say in two sentences what this agent does now and which version it is.
   END IF
7. SET `questions` TO one short message with at most 3 questions about purpose, users, limits and existing code that `request` leaves open, each with a proposed default, or "" if nothing important is unclear
8. IF `questions` IS NOT EMPTY THEN
       ASK USER `questions`
       SAVE answer INTO VARIABLE `details`
   ELSE
       SET `details` TO "no further details"
   END IF
9. IF `request` or `details` asks to wrap an existing repository or program THEN
       INVOKE SKILL "wrap-existing-project" USING context { "request": `request`, "details": `details`, "now": "Only section 1 now: put the code in artifacts/ and study it. Write no project files and run no builds. Reply with your notes: what it does, entry points with their real arguments, inputs, outputs, what must be installed or running, network use, run time, licence, usage rules." }
       SAVE skill_output INTO VARIABLE `repo_notes`
   ELSE
       SET `repo_notes` TO "no existing code"
   END IF

STEP 2: PLAN
1. INVOKE SKILL "create-agent" USING context { "request": `request`, "details": `details`, "repo_notes": `repo_notes`, "now": "Only plan in this step: write no files and run no builds. Reply with the plan itself: the agent's name, its role, its skills, its tools (name, purpose, input JSON, what it calls), its FLOW.md steps, its state files and what must be installed or running." }
2. SAVE skill_output INTO VARIABLE `plan`
3. ASK USER "That is my plan (above). Shall I build it? Say yes, or tell me what to change."
4. WHILE `answer` is not an approval of the plan AT MOST 3 TIMES DO
       SET `plan` TO the plan in `plan`, revised with the changes the user asked for in `answer`
       ASK USER "Here is the revised plan. Shall I build it?\n\n" + `plan`
   END WHILE
5. IF `answer` is not an approval of the plan THEN
       RETURN "Stopped: the plan was not approved. Tell me what you want and we can start again."
   END IF

STEP 3: WRITE
1. INVOKE SKILL "write-agent-md-and-skills" USING context `plan`
2. SET `tools` TO the tools in `plan` as a JSON array of {"name", "purpose"} objects, or [] if it has none
3. FOR EACH `tool` IN `tools` DO
       INVOKE SKILL "write-tool" USING context `tool`
       RETRY UP TO 3 TIMES DO
           Test the tool `tool.name` by hand with shell_exec and realistic stdin JSON, and fix it if it fails.
       END RETRY
   END FOR
4. Write config.json as the project-layout skill says: name (this folder's name), display_name, version (0.1.0 for a new agent; for a changed one, bump PATCH for fixes, MINOR for new abilities, MAJOR for changed behaviour), a one-sentence description, "internet": false only if `plan` says the agent must work offline, the skills and tools written above, and in "requires" every program they need on the machine.
5. IF `plan` describes more than one step for the agent THEN
       INVOKE SKILL "write-flow" USING context `plan`
   END IF

STEP 4: CHECK
1. INVOKE SKILL "build-agent" USING context `plan`
2. EXECUTE tool `agent_build` with payload { "dry_run": true }
3. SAVE result INTO VARIABLE `check`
4. WHILE `check` CONTAINS "error" AT MOST 5 TIMES DO
       Fix every error and warning reported here: `check`
       EXECUTE tool `agent_build` with payload { "dry_run": true }
       SAVE result INTO VARIABLE `check`
   END WHILE
5. IF `check` CONTAINS "error" THEN
       RETURN "the errors that remain in `check` and what the user can do about them"
   END IF
6. EXECUTE tool `fs_list` with payload { "path": "." }
7. IF `result` CONTAINS "FLOW.md" THEN
       EXECUTE tool `owl_review` with payload { "dir": "." }
       SAVE result INTO VARIABLE `review`
       IF `review.makes_sense` IS EQUAL TO false THEN
           ASK USER "Some steps in FLOW.md do not make sense yet:\n\n" + `review.report` + "\nShall I apply these suggestions?"
           IF `answer` is an approval THEN
               Apply the suggestions in `review` to FLOW.md.
               EXECUTE tool `agent_build` with payload { "dry_run": true }
           END IF
       END IF
   END IF

STEP 5: BUILD
1. EXECUTE tool `agent_build` with payload { "dry_run": false }
2. SAVE result INTO VARIABLE `build`
3. EXECUTE tool `agent_inspect` for the .bot file named in `build`
4. RETURN the file name, its size, how to run it, what it asks at startup and what must be installed or running, from `build` and `result`
```
