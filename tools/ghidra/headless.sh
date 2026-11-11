#!/bin/bash
# Ghidra headless with a bigger heap than analyzeHeadless's hardcoded 2G.
# Run with XDG_CONFIG_HOME/XDG_CACHE_HOME pointing into work/ghidra/ to keep a private settings dir.
GH=${GHIDRA_HOME:?set GHIDRA_HOME (see work/ENV.md)}
exec "$GH/support/launch.sh" fg jdk Ghidra-Headless "${GHIDRA_MAXMEM:-9G}" \
  "-XX:ParallelGCThreads=4 -XX:CICompilerCount=2 -Djava.awt.headless=true" \
  ghidra.app.util.headless.AnalyzeHeadless "$@"
