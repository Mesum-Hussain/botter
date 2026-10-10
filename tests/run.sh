#!/bin/sh
# botter / botcore test suite.   make test      (or: sh tests/run.sh)
#
# Needs: a built tree (make), python3, musl-gcc. Uses a throwaway temp dir.
# Optional: BOTTER_TEST_NET=1 adds internet tests (LLM providers, badssl.com);
#           tmux enables the Ctrl-C test.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TESTS=$ROOT/tests
PACK=$ROOT/artifacts/botter_pack
CORE=$ROOT/artifacts/botcore
BOTTER=$ROOT/botter
T=$(mktemp -d "${TMPDIR:-/tmp}/botter-test.XXXXXX")
PASS=0 FAIL=0 SKIP=0
STUB=
# Never touch the user's real key cache; the keyring tests use their own namespace.
export BOTCORE_NO_KEYRING=1
KRNS=botter-test-$$

cleanup() {
    [ -n "$STUB" ] && kill "$STUB" 2>/dev/null
    tmux kill-session -t botter-test 2>/dev/null
    rm -f "$HOME/.botter_test_probe"
    rm -rf "$T"
}
trap cleanup EXIT INT TERM

pass() { PASS=$((PASS + 1)); printf '  \033[32mPASS\033[0m %s\n' "$1"; }
fail() { FAIL=$((FAIL + 1)); printf '  \033[31mFAIL\033[0m %s\n' "$1"; [ $# -gt 1 ] && printf '       %s\n' "$2"; }
skip() { SKIP=$((SKIP + 1)); printf '  \033[33mSKIP\033[0m %s (%s)\n' "$1" "$2"; }
# check NAME FILE PATTERN: pass if FILE matches the extended regex PATTERN
check() { if grep -Eq -- "$3" "$2" 2>/dev/null; then pass "$1"; else fail "$1" "expected /$3/ in: $(head -c 300 "$2" 2>/dev/null | tr '\n' ' ')"; fi; }
check_not() { if grep -Eq -- "$3" "$2" 2>/dev/null; then fail "$1" "unexpected /$3/"; else pass "$1"; fi; }
section() { printf '\n\033[1m%s\033[0m\n' "$1"; }

for f in "$PACK" "$CORE" "$BOTTER"; do
    [ -x "$f" ] || { echo "missing $f: run make first"; exit 2; }
done

# stub_start ARGS...: start the LLM stub, set PORT; records go to $T/rec
stub_start() {
    stub_stop
    rm -rf "$T/rec" "$T/port"
    python3 "$TESTS/stub_llm.py" --portfile "$T/port" --record "$T/rec" "$@" &
    STUB=$!
    i=0
    while [ ! -s "$T/port" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
    PORT=$(cat "$T/port")
}
stub_stop() { [ -n "$STUB" ] && { kill "$STUB" 2>/dev/null; wait "$STUB" 2>/dev/null; }; STUB=; }
# tool_call TOOL JSON: stub that asks for one tool call
tool_call() { stub_start --tool "$1" --args "$2"; }
# shell_call COMMAND: stub that asks for shell_exec(COMMAND)
shell_call() { tool_call shell_exec "$(python3 -c 'import json,sys;print(json.dumps({"command":sys.argv[1]}))' "$1")"; }
# drive BOT WORKDIR [ENV...]: connect to the stub, send one message, quit. Output in $T/out.
drive() {
    bot=$1 wd=$2
    shift 2
    (cd "$wd" && printf '6\nhttp://127.0.0.1:%s/v1\n\nstub\ngo\n/exit\n' "$PORT" |
        timeout 60 env "$@" "$bot" >"$T/out" 2>&1)
}
# mkagent DIR [config.json content]: a minimal agent project
mkagent() {
    mkdir -p "$1"
    echo "You are a test agent." >"$1/agent.md"
    [ $# -gt 1 ] && printf '%s\n' "$2" >"$1/config.json"
    return 0
}
build() { "$PACK" build "$1" "$CORE" "$2" >"$T/build.log" 2>&1; }

# ---------------------------------------------------------------------------
section "Packaging and Owl"

"$PACK" check "$ROOT" "$CORE" >"$T/self.log" 2>&1
check "botter's own project builds with 0 warnings" "$T/self.log" "check passed.* 0 warning"

# Owl (FLOW.md): a valid flow compiles; each kind of mistake is an error with its line and stops the build
mkagent "$T/flow_good"
mkdir -p "$T/flow_good/skills/pitching"
printf -- '---\nname: pitching\ndescription: p\n---\n' >"$T/flow_good/skills/pitching/SKILL.md"
cp "$TESTS/owl_good.md" "$T/flow_good/FLOW.md"
"$PACK" check "$T/flow_good" "$CORE" >"$T/lint.log" 2>&1
check_not "Owl: a valid FLOW.md has no errors or warnings" "$T/lint.log" "FLOW.md"
# sq_bad NAME OLD NEW PATTERN: owl_good.md with OLD replaced by NEW must report PATTERN
sq_bad() {
    rm -rf "$T/flow_bad" "$T/flow_bad.bot"
    cp -r "$T/flow_good" "$T/flow_bad"
    python3 -c 'import sys;p=sys.argv[1];s=open(p).read();assert sys.argv[2] in s;open(p,"w").write(s.replace(sys.argv[2],sys.argv[3],1))' \
        "$T/flow_bad/FLOW.md" "$2" "$3"
    "$PACK" build "$T/flow_bad" "$CORE" "$T/flow_bad.bot" >"$T/lint.log" 2>&1
    check "Owl: $1" "$T/lint.log" "$4"
}
sq_bad "keyword typo is an error with its line" 'INVOKE SKILL' 'INVOCATE SKILL' 'FLOW.md:15: error: unknown keyword INVOCATE; did you mean INVOKE'
if [ -e "$T/flow_bad.bot" ]; then fail "Owl: a flow with errors is not built"; else pass "Owl: a flow with errors is not built"; fi
sq_bad "unknown tool" '`fs_read`' '`fs_reed`' 'no such tool'
sq_bad "skill used without LOAD SKILL" 'LOAD SKILL "pitching"' '' 'add LOAD SKILL "pitching"'
sq_bad "variable used before it is set" '`folder` }' '`fodler` }' '`fodler` is used before it is set'
sq_bad "loop variable used after its loop" 'RETURN "Read "' 'RETURN `f.name` + "Read "' 'only exists inside its FOR EACH'
sq_bad "mismatched END" '        END RETRY' '        END IF' 'END IF, but the RETRY opened on line 19 is still open'
sq_bad "unclosed block" 'END FOR' '' 'this FOR EACH is never closed'
sq_bad "IF without THEN" '"notes.txt" THEN' '"notes.txt"' 'IF reads: IF <condition> THEN'
sq_bad "MCP CONNECT is rejected" 'LOAD SKILL "pitching"' 'LOAD SKILL "pitching"
CONNECT mcp://x AS y' 'CONNECT \(MCP servers\) is not supported'
sq_bad "the file must start with \`\`\`owl" '```owl
---' '---' 'starts with ```owl on its first line; the frontmatter goes inside the block'
sq_bad "the block must be closed" 'RETURN "Read " + `files.length` + " files"
```' 'RETURN "Read " + `files.length` + " files"' 'never closed: end the file with ```'
sq_bad "nothing may follow the block" 'RETURN "Read " + `files.length` + " files"
```' 'RETURN "Read " + `files.length` + " files"
```
More text' 'nothing may follow the closing ```'
sq_bad "Markdown step headings are rejected" 'STEP 1: GATHER' '## STEP 1: GATHER' 'write the step label as Owl, without #: STEP n: NAME'
sq_bad "other Markdown is rejected" 'LOAD SKILL "pitching"' '# My flow
LOAD SKILL "pitching"' 'Markdown \(headings, <!-- comments -->\) is not part of Owl'
sq_bad "STEP labels are numbered in order" 'STEP 2: WORK' 'STEP 3: WORK' 'warning: STEP 3 follows STEP 1'
sq_bad "decisions written as prose are a warning" 'Skip it.' 'if it is empty, skip it' "FLOW.md:17: warning: write 'if' as an Owl statement"

# config.json: checked by the build, shown by list, --version, the start-up line and the system prompt
mkagent "$T/meta" '{"name": "meta-agent", "display_name": "Meta Agent", "version": "1.2.3", "description": "Tests metadata."}'
build "$T/meta" "$T/meta.bot"
check_not "config.json: a valid config builds without config.json warnings" "$T/build.log" "config.json"
"$PACK" list "$T/meta.bot" >"$T/list.log" 2>&1
check "config.json: list shows it" "$T/list.log" "^version +1\.2\.3"
"$T/meta.bot" --version >"$T/ver.log" 2>&1
check "config.json: --version prints the agent's name and version" "$T/ver.log" "^meta-agent 1\.2\.3"
# cfg_bad NAME JSON PATTERN: building with this config.json must report PATTERN
cfg_bad() {
    mkagent "$T/cfg_bad" "$2"
    rm -f "$T/cfg_bad.bot"
    "$PACK" build "$T/cfg_bad" "$CORE" "$T/cfg_bad.bot" >"$T/build.log" 2>&1
    check "config.json: $1" "$T/build.log" "$3"
}
cfg_bad "a version that is not MAJOR.MINOR.PATCH is an error" '{"name": "a", "version": "1.0"}' 'version "1\.0" is not MAJOR\.MINOR\.PATCH'
if [ -e "$T/cfg_bad.bot" ]; then fail "config.json: an invalid config is not built"; else pass "config.json: an invalid config is not built"; fi
cfg_bad "a bad name is an error" '{"name": "My Agent", "version": "1.0.0"}' 'name "My Agent" must be'
cfg_bad "invalid JSON is an error" '{"name": "a", "version": }' 'not a valid JSON object'
cfg_bad "a wrong type is an error" '{"name": "a", "version": "1.0.0", "internet": "yes"}' '"internet" must be true or false'
cfg_bad "an unknown key is a warning" '{"name": "a", "version": "1.0.0", "author": "x"}' 'warning: config.json: unknown key "author"'
cfg_bad "missing name, version, description are warnings" '{"internet": true}' 'warning: config.json: no "name"'
cfg_bad "requires takes command names" '{"name": "a", "version": "1.0.0", "requires": ["rm -rf"]}' '"requires" must be a list of names'
cfg_bad "a listed skill must exist" '{"name": "a", "version": "1.0.0", "skills": ["ghost"]}' 'skill "ghost" is listed but there is no skills/ghost/SKILL.md'
mkagent "$T/cfg_old"
echo '{}' >"$T/cfg_old/agent.json"
"$PACK" check "$T/cfg_old" "$CORE" >"$T/build.log" 2>&1
check "config.json: an old agent.json is reported" "$T/build.log" "agent.json is now config.json"
mkagent "$T/flow_old"
printf '```sqnc\nRETURN "x"\n```\n' >"$T/flow_old/SQNC.md"
"$PACK" check "$T/flow_old" "$CORE" >"$T/build.log" 2>&1
check "an old SQNC.md is reported" "$T/build.log" "SQNC.md is now FLOW.md"
# skills / tools lists choose what is compiled in
mkagent "$T/pick" '{"name": "pick", "version": "1.0.0", "description": "d", "skills": ["keep"], "tools": ["tkeep"], "requires": ["python3"]}'
mkdir -p "$T/pick/skills/keep" "$T/pick/skills/drop" "$T/pick/tools/bin" "$T/pick/tools/doc"
for k in keep drop; do printf -- '---\nname: %s\ndescription: d\n---\n' $k >"$T/pick/skills/$k/SKILL.md"; done
for t in tkeep tdrop; do printf '#!/usr/bin/env python3\nprint(1)\n' >"$T/pick/tools/bin/$t"; chmod +x "$T/pick/tools/bin/$t"; echo '{"description": "d"}' >"$T/pick/tools/doc/$t.json"; done
build "$T/pick" "$T/pick.bot"
"$PACK" list "$T/pick.bot" >"$T/list.log" 2>&1
check "config.json skills: a listed skill is compiled in" "$T/list.log" "skills/keep/SKILL.md"
check_not "config.json skills: an unlisted skill is left out" "$T/list.log" "skills/drop/"
check "config.json tools: a listed tool is compiled in" "$T/list.log" "tools/bin/tkeep$"
check_not "config.json tools: an unlisted tool and its doc are left out" "$T/list.log" "tdrop"
check "config.json: leaving one out is a warning" "$T/build.log" 'skills "drop" is not listed in config.json "skills"'
mkagent "$T/needpy" '{"name": "needpy", "version": "1.0.0", "description": "d"}'
mkdir -p "$T/needpy/tools/bin" "$T/needpy/tools/doc"
printf '#!/usr/bin/env python3\nprint(1)\n' >"$T/needpy/tools/bin/pyt"; chmod +x "$T/needpy/tools/bin/pyt"
echo '{"description": "d"}' >"$T/needpy/tools/doc/pyt.json"
"$PACK" check "$T/needpy" "$CORE" >"$T/build.log" 2>&1
check "config.json: a script tool's interpreter missing from requires is a warning" "$T/build.log" 'runs with python3: add "python3" to "requires"'

# ---------------------------------------------------------------------------
section "Runtime: network modes, sandbox, system prompt (stub LLM)"

mkagent "$T/on" && build "$T/on" "$T/on.bot"
mkagent "$T/off" '{"internet": false}' && build "$T/off" "$T/off.bot"
mkdir -p "$T/ws"

shell_call "echo hello-from-tool"
drive "$T/on.bot" "$T/ws" PATH=/usr/bin:/bin
check "tool loop: shell_exec runs and its output reaches the LLM" "$T/rec/tool.txt" "hello-from-tool"
check "online agent: system prompt says internet access" "$T/rec/system.txt" "You have internet access"
check_not "no FLOW.md: no flow instruction in the system prompt" "$T/rec/system.txt" "driven by its FLOW.md"

shell_call "python3 -c \"import socket;socket.create_connection(('127.0.0.1',$PORT));print('NET-OK')\""
drive "$T/on.bot" "$T/ws"
check "online agent: shell commands reach the network" "$T/rec/tool.txt" "NET-OK"

shell_call "curl -s http://127.0.0.1:1/"
drive "$T/off.bot" "$T/ws"
check "offline agent: network commands are refused" "$T/rec/tool.txt" "blocked: the agent is offline-only"
check "offline agent: system prompt says offline" "$T/rec/system.txt" "You are OFFLINE"

shell_call "python3 -c \"import socket;socket.create_connection(('127.0.0.1',$PORT))\" 2>&1 | tail -1"
drive "$T/off.bot" "$T/ws"
check "offline agent: sockets fail in the kernel, the LLM still works" "$T/rec/tool.txt" "Network is unreachable|Permission denied"

shell_call "echo inside > inside.txt && cat inside.txt"
drive "$T/on.bot" "$T/ws"
if grep -q "Landlock unavailable" "$T/out"; then
    skip "Landlock tests" "kernel has no Landlock"
else
    check "Landlock: writing inside the working directory works" "$T/rec/tool.txt" "^inside"
    shell_call "python3 -c \"open('$HOME/.botter_test_probe','w')\" 2>&1 | tail -1"
    drive "$T/on.bot" "$T/ws"
    check "Landlock: writing to \$HOME is denied" "$T/rec/tool.txt" "Permission denied"
    shell_call "python3 -c \"import os;os.listdir(os.path.expanduser('~'))\" 2>&1 | tail -1"
    drive "$T/on.bot" "$T/ws"
    check "Landlock: listing \$HOME is denied" "$T/rec/tool.txt" "Permission denied"
    shell_call "python3 -c \"import tempfile;f=tempfile.NamedTemporaryFile();f.write(b'x');print('TMP-OK')\""
    drive "$T/on.bot" "$T/ws"
    check "Landlock: /tmp stays writable" "$T/rec/tool.txt" "TMP-OK"

    # Plan mode: drive with /plan typed before the message
    plan_drive() { (cd "$2" && printf '6\nhttp://127.0.0.1:%s/v1\n\nstub\n/plan\ngo\n/exit\n' "$PORT" |
        timeout 60 "$1" >"$T/out" 2>&1); }
    shell_call "echo planned > plan.txt; ls plan.txt 2>&1 | tail -1"
    plan_drive "$T/on.bot" "$T/ws"
    check "Plan mode: shell commands cannot write in the working directory" "$T/rec/tool.txt" "Permission denied|Read-only"
    if [ -e "$T/ws/plan.txt" ]; then fail "Plan mode: no file was created"; else pass "Plan mode: no file was created"; fi
    check "Plan mode: the model is told the mode" "$T/rec/user.txt" "^\\[Mode: PLAN"
    tool_call fs_write '{"path":"plan2.txt","content":"x"}'
    plan_drive "$T/on.bot" "$T/ws"
    check "Plan mode: fs_write is refused" "$T/rec/tool.txt" "PLAN MODE"
    echo READ-OK >"$T/ws/readme.txt"
    shell_call "cat readme.txt"
    plan_drive "$T/on.bot" "$T/ws"
    check "Plan mode: read-only commands still run" "$T/rec/tool.txt" "READ-OK"
fi

# Streaming (Server-Sent Events) and what the user sees
stub_start --think
drive "$T/on.bot" "$T/ws"
check "streaming: the request asks for a stream" "$T/rec/stream.txt" "^stream"
check "streaming: the streamed answer is printed" "$T/out" "Hello there, streamed reply\\."
check_not "streaming: <think> reasoning is not printed as the answer" "$T/out" "pondering|<think>"
shell_call "echo split-args-ok"
drive "$T/on.bot" "$T/ws"
check "streaming: tool call arguments split over chunks are joined" "$T/rec/tool.txt" "split-args-ok"
check "streaming: provider extras in tool calls are kept (Gemini thought_signature)" "$T/rec/calls.txt" "sig-0"
check "user view: a tool shows what it does in plain words" "$T/out" "Running a command"
check_not "user view: tool names are not shown" "$T/out" "shell_exec"
stub_start --think --nostream
drive "$T/on.bot" "$T/ws"
check "streaming: an endpoint that refuses streams still works (plain fallback)" "$T/out" "Hello there, streamed reply\\."
check "streaming: the fallback request is plain" "$T/rec/stream.txt" "^plain"
check_not "streaming fallback: reasoning is not printed" "$T/out" "pondering|<think>"

# Session key cache (kernel keyring, own namespace): 2nd run reconnects without asking
if [ -r /proc/keys ]; then
    shell_call "python3 -c \"import ctypes,os;l=ctypes.CDLL(None,use_errno=True);r=l.syscall(250,0,0,0,0,0);print('KEYCTL',r,os.strerror(ctypes.get_errno()))\""
    (cd "$T/ws" && printf '6\nhttp://127.0.0.1:%s/v1\nsk-cache-test\nstub\ngo\n/exit\n' "$PORT" |
        timeout 60 env -u BOTCORE_NO_KEYRING BOTCORE_KEYRING_NS=$KRNS "$T/on.bot" >"$T/out" 2>&1)
    check "key cache: tool children cannot use the keyring syscalls" "$T/rec/tool.txt" "KEYCTL -1 Function not implemented"
    (cd "$T/ws" && printf 'go\n/exit\n' | timeout 60 env -u BOTCORE_NO_KEYRING BOTCORE_KEYRING_NS=$KRNS "$T/on.bot" >"$T/out" 2>&1)
    check "key cache: the next run reconnects without asking" "$T/out" "saved for this session"
    check_not "key cache: no provider question on the next run" "$T/out" "Select an OpenAI-compatible provider"
    check "key cache: the saved key is sent" "$T/rec/auth.txt" "sk-cache-test"
    (cd "$T/ws" && printf '/forget\n/exit\n' | timeout 60 env -u BOTCORE_NO_KEYRING BOTCORE_KEYRING_NS=$KRNS "$T/on.bot" >"$T/out" 2>&1)
    check "key cache: /forget removes the saved entries" "$T/out" "Forgot 3 saved"
    (cd "$T/ws" && printf '' | timeout 60 env -u BOTCORE_NO_KEYRING BOTCORE_KEYRING_NS=$KRNS "$T/on.bot" >"$T/out" 2>&1)
    check "key cache: after /forget the provider is asked again" "$T/out" "Select an OpenAI-compatible provider"
else
    skip "key cache tests" "kernel has no keyring"
fi

mkagent "$T/skilled"
mkdir -p "$T/skilled/skills/pitching"
printf -- '---\nname: pitching\ndescription: Write one specific idea per lead. Read before pitching.\n---\n# Pitching\n' >"$T/skilled/skills/pitching/SKILL.md"
echo "# stale" >"$T/skilled/manifest.md"
build "$T/skilled" "$T/skilled.bot"
check "leftover manifest.md is reported and not embedded" "$T/build.log" "manifest.md is no longer used"
stub_start --text /dev/null
drive "$T/skilled.bot" "$T/ws"
check "system prompt lists skills with their frontmatter descriptions" "$T/rec/system.txt" "skills/pitching/SKILL.md: Write one specific idea per lead"

mkagent "$T/flowagent"
cp -r "$T/flow_good/FLOW.md" "$T/flow_good/skills" "$T/flowagent/"
build "$T/flowagent" "$T/flow.bot"
stub_start --text /dev/null
(cd "$T/ws" && printf '6\nhttp://127.0.0.1:%s/v1\n\nstub\ngo\nhello after the flow\n/exit\n' "$PORT" |
    timeout 60 "$T/flow.bot" >"$T/out" 2>&1)
check "FLOW.md present: the flow starts by itself" "$T/out" "Which folder\\?"
check "FLOW.md present: system prompt explains the [Owl] steps" "$T/rec/system.txt" "session is driven by its FLOW.md flow"

stub_start --text /dev/null
drive "$T/meta.bot" "$T/ws"
check "config.json: the agent shows its display name and version at start" "$T/out" "Meta Agent.*1\.2\.3.*Tests metadata"
check "config.json: the model knows its name and version" "$T/rec/system.txt" 'You are the agent "Meta Agent", version 1\.2\.3\. Tests metadata\.'
mkagent "$T/needcmd" '{"name": "needcmd", "version": "1.0.0", "description": "d", "requires": ["definitely-not-installed-cmd"]}'
build "$T/needcmd" "$T/needcmd.bot"
drive "$T/needcmd.bot" "$T/ws"
check "config.json requires: missing programs are reported at start" "$T/out" "not installed here: definitely-not-installed-cmd"

# Owl interpreter: botcore runs the structure; only prose goes to the LLM
mkagent "$T/sqagent"
cp "$TESTS/owl_run.md" "$T/sqagent/FLOW.md"
build "$T/sqagent" "$T/sq.bot"
sq_drive() {
    rm -rf "$T/sqws"; mkdir -p "$T/sqws"
    (cd "$T/sqws" && printf '6\nhttp://127.0.0.1:%s/v1\n\nstub\nAlice\n/exit\n' "$PORT" |
        timeout 60 "$T/sq.bot" >"$T/out" 2>&1)
}
stub_start --owl yes
sq_drive
check "Owl: ASK USER shows the question" "$T/out" "What is your name\\?"
check "Owl: RETURN joins the answer, a prose decision and .length" "$T/out" "Hi Alice, mood happy, 3 items"
check "Owl: STEP headings are shown" "$T/out" "Step 2 · WORK"
check "Owl: EXECUTE runs the tool itself" "$T/out" "Running a command"
check "Owl: RETRY repeats a failing step" "$T/out" "Retrying \\(2 of 2\\)"
check "Owl: SAVE TO FILE writes the value" "$T/sqws/out/items.json" '"b"'
n=$(grep -c "" "$T/rec/owl.txt" 2>/dev/null)
if [ "$n" = 2 ]; then pass "Owl: only prose reaches the LLM (2 calls for the whole flow)"; else fail "Owl: only prose reaches the LLM (2 calls for the whole flow)" "LLM calls: $n"; fi
check "Owl: FOR EACH + IF run the plain-English step for the matching item" "$T/rec/owl.txt" "line 16\\] Mention the letter"
check "Owl: a prose condition is decided by the LLM" "$T/rec/owl.txt" "Decide whether this condition is true right now: the user seems happy"
stub_start --owl no
sq_drive
check "Owl: the LLM's 'no' takes the ELSE branch" "$T/out" "Hi Alice, mood sad"

# owl_review: the LLM's logic check of a project's FLOW.md (builder agents only)
mkagent "$T/builder" '{"builder": true}'
build "$T/builder" "$T/builder.bot"
mkdir -p "$T/revws"
cp "$TESTS/owl_run.md" "$T/revws/FLOW.md"
echo "An agent that greets people." >"$T/revws/agent.md"
tool_call owl_review '{}'
drive "$T/builder.bot" "$T/revws"
check "owl_review: the verdict reaches the agent" "$T/rec/tool.txt" '"makes_sense":false'
check "owl_review: the report says why and what to do instead" "$T/rec/tool.txt" "why: reads a file that is never written"
check "owl_review: the reviewer gets FLOW.md with line numbers and agent.md" "$T/rec/review.txt" "An agent that greets people"
tool_call owl_review '{}'
drive "$T/on.bot" "$T/revws"
check "owl_review: not available to ordinary agents" "$T/rec/tool.txt" "unknown tool 'owl_review'"

shell_call "echo no-curl-needed"
drive "$T/on.bot" "$T/ws" PATH=/nonexistent
check "runs with no curl (or anything) on PATH" "$T/rec/tool.txt" "no-curl-needed"

mkdir -p "$T/proj" && echo "You are built by the test." >"$T/proj/agent.md"
tool_call agent_build '{}'
drive "$BOTTER" "$T/proj" BOTTER_TUI=0
check "botter: agent_build builds a .bot from inside the sandbox" "$T/rec/tool.txt" "wrote .*proj\\.bot"
[ -x "$T/proj/proj.bot" ] && pass "botter: the built agent is executable" || fail "botter: the built agent is executable"

# ---------------------------------------------------------------------------
section "Pack integrity"

shell_call "echo intact"
drive "$CORE" "$T/ws"
check_not "plain botcore (no pack) still starts" "$T/out" "truncated|damaged"
cp "$T/on.bot" "$T/trunc.bot" && truncate -s -100 "$T/trunc.bot"
drive "$T/trunc.bot" "$T/ws"
check "truncated .bot refuses to start with a clear message" "$T/out" "this agent file is truncated: it should be [0-9]+ bytes"
cp "$T/on.bot" "$T/grown.bot" && printf 'junk' >>"$T/grown.bot"
drive "$T/grown.bot" "$T/ws"
check ".bot with extra bytes appended is reported as damaged" "$T/out" "this agent file is damaged"
cp "$T/proj/proj.bot" "$T/trunc2.bot" && truncate -s -40 "$T/trunc2.bot"
drive "$T/trunc2.bot" "$T/ws"
check "agent built by botter (runtime taken from botter) detects truncation" "$T/out" "truncated"
drive "$T/proj/proj.bot" "$T/ws"
check "agent built by botter runs" "$T/out" "Connected"

# ---------------------------------------------------------------------------
section "HTTP client (local servers)"

if ! musl-gcc -Os -static -I"$ROOT/tools/src/botcore/src" -I"$ROOT/tools/src/botcore/lib/bearssl/inc" \
    -I"$ROOT/tools/src/botcore/lib/ca" -o "$T/http_client" "$TESTS/http_client.c" \
    "$ROOT/tools/src/botcore/src/http.c" "$ROOT/tools/src/botcore/obj-musl/libbearssl.a" 2>"$T/cc.log"; then
    fail "build the HTTP test client" "$(head -c 300 "$T/cc.log")"
else
    python3 "$TESTS/http_servers.py" "$T/hport" &
    HS=$!
    i=0
    while [ ! -s "$T/hport" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
    OP=$(sed -n 1p "$T/hport")
    PP=$(sed -n 2p "$T/hport")
    H=$T/http_client
    $H "http://127.0.0.1:$OP/plain" >"$T/h"
    check "Content-Length body" "$T/h" '"plain":"ok"'
    $H "http://127.0.0.1:$OP/chunked" >"$T/h"
    check "chunked body is decoded" "$T/h" '\{"a":"chunked ok"\}'
    $H "http://127.0.0.1:$OP/post" sk-test '{"x":[1,"é"]}' >"$T/h"
    check "POST: status, Bearer header and UTF-8 body" "$T/h" 'status=201.*|"auth": "Bearer sk-test"'
    check "POST: body echoed intact" "$T/h" '"got": \{"x": \[1, "\\u00e9"\]\}'
    HTTPS_PROXY="http://u:wrong@127.0.0.1:$PP" $H https://example.invalid/ >"$T/h"
    check "proxy refusal is reported (407)" "$T/h" "refused the tunnel \\(HTTP 407\\)"
    $H "ftp://x/" >"$T/h"
    check "non-http URLs are rejected" "$T/h" "invalid URL"
    $H "http://127.0.0.1:1/" >"$T/h"
    check "connection refused is reported" "$T/h" "could not connect to 127.0.0.1 port 1"
    kill "$HS" 2>/dev/null

    if [ "${BOTTER_TEST_NET:-0}" = 1 ]; then
        section "HTTP client (internet: BOTTER_TEST_NET=1)"
        for u in https://generativelanguage.googleapis.com/v1beta/openai/models https://api.openai.com/v1/models \
            https://api.groq.com/openai/v1/models https://openrouter.ai/api/v1/models; do
            $H "$u" bogus >"$T/h"
            check "TLS + HTTP to $u" "$T/h" "rc=0 status=(200|400|401|403)"
        done
        $H https://self-signed.badssl.com/ >"$T/h"
        check "self-signed certificate refused" "$T/h" "not trusted"
        $H https://expired.badssl.com/ >"$T/h"
        check "expired certificate refused" "$T/h" "expired"
        $H https://wrong.host.badssl.com/ >"$T/h"
        check "wrong host name refused" "$T/h" "does not match the host name"
        python3 "$TESTS/http_servers.py" "$T/hport2" &
        HS=$!
        i=0
        while [ ! -s "$T/hport2" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
        PP=$(sed -n 2p "$T/hport2")
        HTTPS_PROXY="http://u:p%20w@127.0.0.1:$PP" $H https://api.groq.com/openai/v1/models bogus >"$T/h"
        check "HTTPS through an authenticated CONNECT proxy" "$T/h" "rc=0 status=401"
        kill "$HS" 2>/dev/null
    else
        skip "internet HTTPS tests" "set BOTTER_TEST_NET=1"
    fi
fi

# ---------------------------------------------------------------------------
section "Interactive (tmux)"
if command -v tmux >/dev/null 2>&1; then
    stub_start --slow
    tmux new-session -d -s botter-test -x 100 -y 20 -c "$T/ws" "$T/on.bot"
    sleep 0.5
    for k in 6 Enter "http://127.0.0.1:$PORT/v1" Enter Enter stub Enter; do
        tmux send-keys -t botter-test "$k"
        sleep 0.3
    done
    sleep 0.8
    tmux send-keys -t botter-test "hello" Enter
    sleep 1.5
    tmux send-keys -t botter-test C-c
    sleep 1
    tmux capture-pane -t botter-test -p >"$T/tmux.txt"
    tmux kill-session -t botter-test 2>/dev/null
    check "Ctrl-C cancels an in-flight LLM request within a second" "$T/tmux.txt" "\\(interrupted\\)"

    # Botter's TUI: its flow asks first; the first message answers it, later ones are chat tagged [N]
    stub_start --owl yes
    tmux new-session -d -s botter-test -x 100 -y 30 -c "$T/ws" "env BOTTER_THEME=dark $BOTTER"
    sleep 0.8
    for k in 6 Enter "http://127.0.0.1:$PORT/v1" Enter Enter stub Enter; do
        tmux send-keys -t botter-test "$k"
        sleep 0.3
    done
    sleep 0.8
    tmux send-keys -t botter-test "first message" Enter
    sleep 1
    tmux send-keys -t botter-test "second message" Enter
    sleep 1.2
    tmux capture-pane -t botter-test -p >"$T/tui.txt"
    tmux kill-session -t botter-test 2>/dev/null
    check "TUI: messages reach the model tagged with their number" "$T/rec/user.txt" "^\\[2\\] second message"
    check "TUI: the model is told what the numbers mean" "$T/rec/system.txt" "numbers each user message and your reply"
    check_not "TUI: header is gone after the first message" "$T/tui.txt" "the agent that builds agents"
    if head -4 "$T/tui.txt" | grep -q "What agent should I build"; then pass "TUI: the flow's question stays at the top when the header goes"; else fail "TUI: the flow's question stays at the top when the header goes"; fi
    check "TUI: the first message answered the flow" "$T/rec/owl.txt" "Answer the question"

    # TUI after a tool call: streamed reply shown once, tool internals hidden
    stub_start --owl yes --tool shell_exec --args '{"command":"echo hi"}'
    tmux new-session -d -s botter-test -x 100 -y 30 -c "$T/ws" "env BOTTER_THEME=dark $BOTTER"
    sleep 0.8
    for k in 6 Enter "http://127.0.0.1:$PORT/v1" Enter Enter stub Enter; do
        tmux send-keys -t botter-test "$k"
        sleep 0.3
    done
    sleep 0.8
    tmux send-keys -t botter-test "hello" Enter
    sleep 1
    tmux send-keys -t botter-test "run it" Enter
    sleep 1.5
    tmux capture-pane -t botter-test -p >"$T/tui.txt"
    tmux kill-session -t botter-test 2>/dev/null
    n=$(grep -c "^ *done *$" "$T/tui.txt")
    if [ "$n" -eq 1 ]; then pass "TUI: a streamed reply is shown once"; else fail "TUI: a streamed reply is shown once" "seen $n times"; fi
    check_not "TUI: tool names are hidden unless ctrl+o" "$T/tui.txt" "shell_exec"
else
    skip "Ctrl-C test" "tmux not installed"
fi

stub_stop
printf '\n%d passed, %d failed, %d skipped\n' "$PASS" "$FAIL" "$SKIP"
[ "$FAIL" -eq 0 ]
