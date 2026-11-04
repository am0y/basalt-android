# Contributing

This is a small, version-specific project. A good issue or PR saves a lot of
guesswork: include the Roblox version/build, Android version, ABI, whether it
was a release or development build, what you tried, and what happened.

- For a crash or missing editor, attach the smallest useful log excerpt and
  exact steps to reproduce it. Remove account information and private scripts.
- For VM offsets or hooks, explain how you checked the new layout and which
  client build it belongs to. A matching instruction pattern alone isn't
  proof that an offset is safe to ship.
- Keep patches focused. Run the offline checks in [BUILDING.md](BUILDING.md);
  say which device tests you could run and which you couldn't.
- Prefer readable source and comments that explain surprising behavior. Don't
  submit obfuscated code or claims of compatibility without a reproduction.
- Open an issue before a large port or architecture change so work doesn't
  overlap. Small fixes and documentation PRs can go straight to review.

Never include Roblox APKs, decoded client classes, downloaded scripts, signing
keys, account credentials, backend tokens, or private logs in an issue or PR.
Check `git diff --cached` before committing; `.gitignore` cannot protect files
that you force-add or previously committed.
After staging, `python3 scripts/check_public_tree.py` catches common export
mistakes. It does not replace a manual review of the staged diff.

Contributions to Basalt-owned source are under [MIT](LICENSE). Keep existing
notices for third-party files and artwork; see [NOTICE.md](NOTICE.md).
