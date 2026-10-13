# The Last Guardian PC — fork direction

**Current stage:** defining the new focus. The inherited implementation remains
specialized for Bloodborne; no The Last Guardian execution milestone has been
validated in this fork.

## Initial priorities

1. **Characterize the target executable.** Identify the edition, version, metadata,
   modules, and imports from your own The Last Guardian dump before defining
   supported IDs and hashes.
2. **Map Bloodborne dependencies.** Review image preparation, game validation,
   fixed addresses, hooks, patches, the GPU profile, and launcher options.
3. **Adapt loading and the runtime.** Implement and verify what The Last Guardian's
   executable needs, without reusing Bloodborne patches or addresses.
4. **Validate the first boot and rendering.** Record the environment, game version,
   logs, and reproducible failures before claiming compatibility.
5. **Validate the basic experience.** Check audio, controls, and saving;
   optimization, upscaling, and distribution follow basic execution.

Internal names such as `bbport`, `BB_*`, and existing artifacts remain at this stage.
They identify the inherited foundation and do not represent support for the new game.

## Bloodborne technical history

The [original upstream roadmap (Russian)](UPSTREAM_ROADMAP.ru.md) is preserved
separately. Its results, completed items, and plans refer to Bloodborne, not
The Last Guardian's progress. The full upstream documentation is available in
[UPSTREAM_README.md](UPSTREAM_README.md).
