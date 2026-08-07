# Agent Log — cross-LLM action journal

This directory is the shared memory between the different LLMs/agents that work
on this repository (Kimi Code, Gemini, Cursor agents, …). **Every agent session
must append one dated file here before finishing**, so the next agent can pick
up without re-discovering state.

## File naming

```
YYYY-MM-DD-<agent>-<short-topic>.md
```

One file per session. Use the session's local date. Append-only: never rewrite
another agent's entry; if you need to correct something, say so in your own entry.

## Required sections

1. **Goal** — what the user asked for.
2. **What was done** — commits (hashes), files created/modified, commands run
   with non-obvious effects (rebases, force-pushes, submodule changes).
3. **State at handoff** — branch checked out, build/test status, anything
   uncommitted or unpushed, open PRs touched.
4. **Gotchas for the next agent** — dead ends, half-finished work, things that
   look done but aren't, user process rules (e.g. "don't commit unless asked").

## Predecessor

`handoff.md` (repo root, 2026-07-30, Cursor agent) predates this directory and
covers the HostSim live-telemetry / PR #3 work. Treat it as the first entry.

## Index

| Date | Agent | Entry |
|------|-------|-------|
| 2026-07-30 | Cursor agent | `../handoff.md` (root) |
| 2026-08-04 | Gemini 3.6 Flash | `2026-08-04-gemini-windows-fixes-fpga-foc.md` |
| 2026-08-05 | Kimi Code | `2026-08-05-kimi-fork-sync-rebase-audit.md` |
| 2026-08-05 | Kimi Code | `2026-08-05-audit-report.md` (module audit findings) |
