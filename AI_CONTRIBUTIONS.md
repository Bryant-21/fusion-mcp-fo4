# AI-assisted contributions

AI tools are welcome. A person stays accountable for every change.

- **A human submits.** Only a person pushes or opens a pull request, confirming they may contribute it
  under this repository's license. Never list an AI as author, `Co-authored-by:` or `Signed-off-by:`.
- **Credit the tools** with one trailer per agent: `Assisted-by: AGENT:MODEL [TOOL ...]`, for example
  `Assisted-by: Claude Code:claude-opus-5-5 modkit ast-grep`. TOOL lists analysis tools that shaped the
  change (modkit, ast-grep, Ghidra, RenderDoc), not git, compilers or editors. Line autocomplete needs none.
- **Understand it.** You needn't read every line, but you must know what the change does and how, and be
  able to answer questions about it in review.
- **Test it.** Build it and run the tests; check runtime behaviour in Fallout 4, since host tests don't
  prove a hook, record or shader works in game.
- **Keep it clean.** No pasted decompiled Bethesda code, other mods' sources or game assets: use names,
  offsets and Address Library IDs.
- **Own it.** Bugs, crashes and broken saves it causes are yours. Reviewers may decline a change its
  submitter can't explain.
