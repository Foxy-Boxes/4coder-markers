# 4coder-markers: the service coding agents post markers and stories to.
#   make            build ./4coder-markers
#   make install    copy it to $(PREFIX)/bin (default ~/.local)

CXX ?= c++
PREFIX ?= $(HOME)/.local
VERSION ?= $(shell git describe --always --dirty 2>/dev/null || echo src)
CXXFLAGS ?= -O2
CXXFLAGS += -std=c++11 -Wall -Wno-unused-function -DFCODER_MARKERS_VERSION='"$(VERSION)"'

SOURCES = src/4coder_markers.cpp protocol/4coder_agent_markers_protocol.h protocol/4coder_agent_stories_protocol.h

4coder-markers: $(SOURCES)
	$(CXX) $(CXXFLAGS) src/4coder_markers.cpp -o $@

install: 4coder-markers
	mkdir -p $(PREFIX)/bin
	cp 4coder-markers bin/4coder-markers-remote $(PREFIX)/bin/

clean:
	rm -f 4coder-markers

.PHONY: install clean
