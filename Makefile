# Build the botter agent (this project is itself an agent project, see manifest.md).
#
#   make            -> ./botter     (static x86-64 executable: botcore + embedded pack)
#   make tools      -> tools/bin/*  (static musl executables from tools/src/agent_tools)
#   make artifacts  -> artifacts/botcore (static runtime) + artifacts/botter_pack (packer)
#   make clean
#
# Build-time only; none of this is embedded. Needs gcc and musl-gcc (Fedora: musl-gcc musl-libc-static).

BOTCORE_DIR = tools/src/botcore
PACK_DIR    = tools/src/pack
TOOL_NAMES  = agent_manifest agent_build agent_inspect
TOOL_BINS   = $(addprefix tools/bin/,$(TOOL_NAMES))
CJSON       = $(BOTCORE_DIR)/lib/cjson

AGENT_FILES = $(shell find manifest.md agent.md skills tools/doc -type f 2>/dev/null)

botter: artifacts/botcore artifacts/botter_pack $(TOOL_BINS) $(AGENT_FILES)
	artifacts/botter_pack build . artifacts/botcore $@

tools: $(TOOL_BINS)

artifacts: artifacts/botcore artifacts/botter_pack

artifacts/botcore: $(wildcard $(BOTCORE_DIR)/src/*.c $(BOTCORE_DIR)/src/*.h)
	$(MAKE) -C $(BOTCORE_DIR) release
	@mkdir -p artifacts
	cp $(BOTCORE_DIR)/botcore-static $@

artifacts/botter_pack: $(PACK_DIR)/botter_pack.c
	@mkdir -p artifacts
	musl-gcc -Os -static -std=gnu11 -Wall -Wextra -o $@ $<
	strip $@

AGENT_TOOLS = tools/src/agent_tools/agent_tools

$(AGENT_TOOLS): tools/src/agent_tools/agent_tools.c $(PACK_DIR)/botter_pack.c $(CJSON)/cJSON.c
	musl-gcc -Os -static -std=gnu11 -Wall -Wextra -Wno-unused-function -Wno-format-truncation -I$(CJSON) -ffunction-sections -fdata-sections \
		-Wl,--gc-sections -o $@ tools/src/agent_tools/agent_tools.c $(CJSON)/cJSON.c -lm
	strip $@

# One executable, three tool names (the tool runner starts it under its tool name).
$(TOOL_BINS): $(AGENT_TOOLS)
	@mkdir -p tools/bin
	cp $< $@

clean:
	rm -rf artifacts botter tools/bin $(AGENT_TOOLS)
	$(MAKE) -C $(BOTCORE_DIR) clean

.PHONY: tools artifacts clean
