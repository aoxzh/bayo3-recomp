# Contributing

Thanks for helping with Bayonetta 3 Recomp. The project is experimental: no complete playthrough has been verified, and a passing test or a screenshot is not evidence that the game is finished. Before changing anything that depends on the game, read [BUILD.md](BUILD.md) for prerequisites, the game-free test path and the shader inputs that are still unresolved.

## Start with a report someone can reproduce

Describe what happened, what you expected, the shortest set of steps, and the exact command. Add the game version when it matters, the operating system and architecture, and the versions of the compiler and relevant tools. Include the revision if the checkout has one. For graphics problems, name the GPU and driver, the scene, and what you actually saw. Say where the evidence comes from: a standalone test, a generated build, an image, or an interactive run. A title screen or cutscene does not demonstrate gameplay, and it certainly does not demonstrate a full playthrough.

Before sharing anything, strip paths, usernames, logs and configuration. Never include keys, firmware, game dumps, saves, game-derived analysis, or generated C++/binaries/shaders. Preparation and shader manifests can leak local paths and hashes, so read them instead of attaching them wholesale. Local acceptance records from `scripts/b3_accept.sh --record` can contain host paths and run metadata; `docs/acceptance/` is ignored by default and those records still need review. Visual regression captures can record usernames, hostnames, local paths and `NX_` environment variables: keep the reports private and review any excerpt. Do not publish screenshots without checking the rights involved. The ignore rules help, but they do not prevent every accidental disclosure, so inspect the files and changes yourself.

## Making a change

- Keep changes focused. Explain the problem, and preserve existing copyright, license and provenance notices. Check [LICENSING.md](LICENSING.md) before copying or adapting external code, and identify the exact upstream source, version and license.
- Use test data you can share. Keep game inputs and derived outputs local. `--inspect-only` writes private files, and shader generation takes externally supplied inputs; neither is a game-free test.
- Run the smallest meaningful test for your change, then the broader checks where you can. The standalone CMake tests need no game, but which tests get registered depends on the platform and optional dependencies. Report commands, passes, skips and failures as they are.
- If your change needs a real game run, state exactly what you verified and what still fails. Do not turn a successful build, unit test or cutscene into a broader playability claim. Cross-platform behavior needs evidence from that platform and that revision.
- Before proposing a change, look through the diff and any new files for private data, generated output and license notices. In a Git checkout, `git status` and `git diff` help; this curated export may not include Git metadata at all.

If a step cannot be reproduced without private data, say which input is missing and what you did verify. Do not supply copyrighted inputs to fill the gap. Nothing here promises that every report can be independently reproduced from this source export alone.
