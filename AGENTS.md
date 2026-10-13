# Repository instructions

These instructions apply to the entire repository.

## Project context

- This is LuizDev1's individual Windows project for The Last Guardian, based on
  `deadinside28/bloodborne_pc` (bbport). Spexxl develops a separate Linux effort;
  do not treat this branch as work submitted to or maintained by that project.
- The inherited foundation uses native x86-64 execution, a PS4 system runtime,
  and a Vulkan renderer derived from shadPS4. Its starting platform is Linux x86-64.
- The implementation still contains Bloodborne-specific executable validation,
  patches, hooks, addresses, graphics assumptions, and launcher options.
- A Windows entry diagnostic reaches original instructions; it is not a playable
  runtime. See `docs/TARGET_INSPECTION.md` for the exact observed milestone. Never carry
  Bloodborne compatibility claims, benchmarks, or completed milestones over to it.
- Consult `docs/ROADMAP.md` for the fork's direction and `docs/UPSTREAM_README.md`
  for inherited technical documentation. Upstream history describes Bloodborne.

## Language and documentation

- Write new or updated documentation, code comments, and project-facing text in English.
- Respond to the user in Portuguese unless they request another language.
- Keep `README.md` concise and useful to ordinary users: introduction, downloads,
  usage information, game files, support, credits, and license.
- Keep architecture, repository layout, implementation plans, and development
  status in `AGENTS.md` or `docs/`, rather than in the main README.
- Do not invent releases, installation steps, supported hardware, game IDs,
  executable hashes, or compatibility. Publish usage instructions only when verified.
- Preserve upstream attribution, licenses, and historical documentation. Label
  inherited material clearly so users do not mistake it for The Last Guardian guidance.

## Working with the inherited code

- Keep changes focused on the requested scope and preserve existing user edits.
- Retain internal names such as `bbport`, `BB_*`, `bbport.ini`, `bb-probe`, and
  `libbbgpu.so` until a coordinated rename is explicitly requested or required.
- Never apply Bloodborne patches, guest addresses, or executable hashes to
  The Last Guardian without independent validation against its executable.
- Inspect game-specific assumptions before reusing runtime or graphics behavior.
- Keep game-specific GPU behavior in `gpu/games/` where practical.
- Preserve third-party licenses and treat vendored code and submodules carefully.
- Do not add game executables, game assets, dumps, credentials, or generated binaries.

## Repository map

- `src/`: executable loader and PS4 system runtime.
- `scripts/`: image preparation, module linking, validation, and patches.
- `gpu/`: Vulkan renderer, shader recompilation, and game profiles.
- `launcher/`, `packaging/`: launcher and distribution tooling.
- `patches/`: inherited Bloodborne patches.
- `tests/`, `tools/`: tests and development utilities.
- `docs/`: technical references, roadmap, and upstream history.

## Verification

- For documentation-only changes, check local Markdown links and run `git diff --check`.
- For code changes, run relevant tests and build checks; report what actually ran
  and any limitations. The inherited commands include `bash build.sh --test` and
  `python3 -m unittest discover -s tests`; dependencies are described in `shell.nix`.
- Building the infrastructure or passing inherited tests does not prove
  The Last Guardian boots or plays. Game compatibility requires evidence from the target game.

