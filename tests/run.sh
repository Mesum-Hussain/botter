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
# mkagent DIR [agent.json content]: a minimal agent project
mkagent() {
    mkdir -p "$1"
    echo "You are a test agent." >"$1/agent.md"
    [ $# -gt 1 ] && printf '%s\n' "$2" >"$1/agent.json"
    return 0
}
build() { "$PACK" build "$1" "$CORE" "$2" >"$T/build.log" 2>&1; }

OML_HDR=$(sed -n '/^<!-- OML v1: Write/p' "$ROOT/skills/write-flow/SKILL.md")

# ---------------------------------------------------------------------------
section "Packaging and OML lint"

"$PACK" check "$ROOT" "$CORE" >"$T/self.log" 2>&1
check "botter's own project builds with 0 warnings" "$T/self.log" "check passed: .* 0 warning"

mkagent "$T/flow_good"
printf '%s\n\n# Flow\n\n```python oml\nset x = 1\nif x is 1:\n    ask user to confirm\nelse:\n    return "no"\nfor lead in leads:\n    retry 3 times:\n        send it\nreturn x\n```\n' "$OML_HDR" >"$T/flow_good/flow.md"
"$PACK" check "$T/flow_good" "$CORE" >"$T/lint.log" 2>&1
check_not "valid fenced flow.md has no flow warnings" "$T/lint.log" "flow.md"

mkagent "$T/flow_bad"
printf '%s\n\n# Flow\n\n```oml\nif broken\n    x\nelif later:\n  y\n```\n' "$OML_HDR" >"$T/flow_bad/flow.md"
"$PACK" check "$T/flow_bad" "$CORE" >"$T/lint.log" 2>&1
check "missing ':' reported with its file line" "$T/lint.log" "flow.md line 6: if/elif/else/for/while lines must end"
check "unexpected indent reported" "$T/lint.log" "line 7: unexpected indent"
check "bad indentation reported" "$T/lint.log" "line 9: indentation must be a multiple of 4"

mkagent "$T/flow_nofence"
printf '%s\n\n# Flow\n\nset x = 1\n' "$OML_HDR" >"$T/flow_nofence/flow.md"
"$PACK" check "$T/flow_nofence" "$CORE" >"$T/lint.log" 2>&1
check "unfenced flow.md warns about the fence" "$T/lint.log" "inside a \`\`\`oml fenced block"

# ---------------------------------------------------------------------------
section "Runtime: network modes, sandbox, system prompt (stub LLM)"

mkagent "$T/on" && build "$T/on" "$T/on.bot"
mkagent "$T/off" '{"offline": true}' && build "$T/off" "$T/off.bot"
mkdir -p "$T/ws"

shell_call "echo hello-from-tool"
drive "$T/on.bot" "$T/ws" PATH=/usr/bin:/bin
check "tool loop: shell_exec runs and its output reaches the LLM" "$T/rec/tool.txt" "hello-from-tool"
check "online agent: system prompt says internet access" "$T/rec/system.txt" "You have internet access"
check_not "no flow.md: no flow instruction in the system prompt" "$T/rec/system.txt" "read flow.md"

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
cp "$T/flow_good/flow.md" "$T/flowagent/"
build "$T/flowagent" "$T/flow.bot"
stub_start --text /dev/null
drive "$T/flow.bot" "$T/ws"
check "flow.md present: system prompt tells the agent to follow it" "$T/rec/system.txt" "read flow.md: it is the flow of this session"

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

    # Botter's TUI: messages reach the model tagged [N]; the header is gone after the first message
    stub_start --text /dev/null
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
    if head -3 "$T/tui.txt" | grep -q "first message"; then pass "TUI: the first message is at the top"; else fail "TUI: the first message is at the top"; fi
else
    skip "Ctrl-C test" "tmux not installed"
fi

stub_stop
printf '\n%d passed, %d failed, %d skipped\n' "$PASS" "$FAIL" "$SKIP"
[ "$FAIL" -eq 0 ]
