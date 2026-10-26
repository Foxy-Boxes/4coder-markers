# 4coder-markers: the service coding agents post markers and stories to.
#   make            build ./4coder-markers
#   make install    copy it to $(PREFIX)/bin (default ~/.local)
#   make install-skill   install the explain-code skill for Claude Code, using
#                   the installed binary and PORT (default 7341)

CXX ?= c++
PREFIX ?= $(HOME)/.local
PORT ?= 7341
SKILLS ?= $(HOME)/.claude/skills
VERSION ?= $(shell git describe --always --dirty 2>/dev/null || echo src)
CXXFLAGS ?= -O2
CXXFLAGS += -std=c++11 -Wall -Wno-unused-function -DFCODER_MARKERS_VERSION='"$(VERSION)"'

SOURCES = src/4coder_markers.cpp protocol/4coder_agent_markers_protocol.h protocol/4coder_agent_stories_protocol.h

4coder-markers: $(SOURCES)
	$(CXX) $(CXXFLAGS) src/4coder_markers.cpp -o $@

install: 4coder-markers
	mkdir -p $(PREFIX)/bin
	cp 4coder-markers bin/4coder-markers-remote $(PREFIX)/bin/

install-skill: install
	mkdir -p $(PREFIX)/share/4coder-markers $(SKILLS)/explain-code
	cp README.md $(PREFIX)/share/4coder-markers/
	sed -e 's|@BIN@|$(PREFIX)/bin/4coder-markers|g' -e 's|@README@|$(PREFIX)/share/4coder-markers/README.md|g' \
	    -e 's|@PORT@|$(PORT)|g' -e 's|@VERSION@|$(VERSION)|g' \
	    skills/explain-code/SKILL.md > $(SKILLS)/explain-code/SKILL.md

clean:
	rm -f 4coder-markers

.PHONY: install install-skill clean
