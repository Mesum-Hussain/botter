---
spec-version: "oml-2"
title: "Botter Session"
author: "Botter"
description: "Design an agent with the user, write its files, check and build it into one .bot file."
---

# Botter Session

LOAD SKILL "create-agent"
LOAD SKILL "project-layout"
LOAD SKILL "write-agent-md-and-skills"
LOAD SKILL "write-tool"
LOAD SKILL "write-flow"
LOAD SKILL "wrap-existing-project"
LOAD SKILL "build-agent"

## STEP 1: UNDERSTAND
1. INVOKE SKILL "create-agent" USING context "the user's first message"
2. IF the first message is a question about Botter or agents THEN
       Answer it briefly.
       RETURN "the answer"
   END IF
3. EXECUTE tool `fs_list` with payload { "path": "." }
4. SAVE result INTO VARIABLE `existing`
5. IF `existing` has agent.md or skills/ THEN
       SET `mode` TO "edit"
       Read agent.md, agent.json, flow.md and the skills that are there.
   ELSE
       SET `mode` TO "new"
   END IF
6. ASK USER "Up to 3 short questions about purpose, users, limits and existing code, each with a proposed default"
7. SAVE answer INTO VARIABLE `needs`
8. IF `needs` asks for an existing repository THEN
       INVOKE SKILL "wrap-existing-project" USING context `needs`
       IF the repository is not on disk THEN
           EXECUTE tool `shell_exec` with payload { "command": "git clone <repo> artifacts/<name>" }
       END IF
   END IF

## STEP 2: PLAN
1. INVOKE SKILL "project-layout" USING context `needs`
2. SAVE skill_output INTO VARIABLE `plan`
3. RETRY UP TO 3 TIMES DO
       ASK USER "Here is the plan (agent.md, skills, tools, flow.md). Approve it, or say what to change." + `plan`
       IF `answer` is not an approval THEN
           Revise `plan` with the user's changes.
       END IF
   END RETRY
4. IF the plan is still not approved THEN
       RETURN "Stopped: the plan was not approved"
   END IF

## STEP 3: WRITE
1. INVOKE SKILL "write-agent-md-and-skills" USING context `plan`
2. FOR EACH `tool` IN `plan.tools` DO
       INVOKE SKILL "write-tool" USING context `tool`
       RETRY UP TO 3 TIMES DO
           EXECUTE tool `shell_exec` with payload { "command": "test the tool by hand with realistic stdin JSON" }
       END RETRY
       IF the tool still fails THEN
           ASK USER "This tool keeps failing. How should I proceed?" + `tool.name`
       END IF
   END FOR
3. IF `needs` asks for an offline agent THEN
       EXECUTE tool `fs_write` with payload { "path": "agent.json", "content": "{\"offline\": true}" }
   END IF
4. IF the agent has more than one step THEN
       INVOKE SKILL "write-flow" USING context `plan`
   END IF

## STEP 4: BUILD
1. INVOKE SKILL "build-agent" USING context `plan`
2. RETRY UP TO 5 TIMES DO
       EXECUTE tool `agent_build` with payload { "dry_run": true }
       Fix every error and warning it reports, including flow.md errors.
   END RETRY
3. IF errors remain THEN
       RETURN "the remaining errors and what the user can do about them"
   END IF
4. EXECUTE tool `agent_build` with payload { "dry_run": false }
5. EXECUTE tool `agent_inspect` with payload { "file": "<name>.bot" }
6. SAVE result INTO VARIABLE `build`
7. RETURN "the file name, its size, how to run it, what it asks at startup and what must be installed or running"
