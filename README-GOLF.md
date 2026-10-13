# golf_fix_pro — unofficial shadPS4 build for Everybody's Golf

This branch is an **unofficial** modification of [shadPS4](https://github.com/shadps4-emu/shadPS4),
made to play **Everybody's Golf (CUSA04687, update 1.17)**.

> It is **not affiliated with or supported by the shadPS4 team**.
> Please do not report problems with this build to the shadPS4 project.
> Open an issue in this fork instead.

## What it changes

- Black scene, out-of-bounds shots and crashes during play
- Character faces (eyes and mouth): GCN `v_writelane` / readlane handling
- Black grass near the hole on afternoon courses: readlane chains through selects
- Horizontal lines in shadows: single-level images are sampled at LOD 0, because
  implicit derivatives inside divergent control flow are undefined in Vulkan
- Crash when reopening the game: shader permutation indices no longer overwrite cached SPIR-V
- Offline play: sign-in dialog result and the empty-store system message
- Faster startup: cached pipelines are built on background threads
- Shader cache stored as loose files, so a forced exit does not lose it
- PS4 Pro mode: tolerate an alternate-tiling image overlap at the same address

Every change is marked with a `golf-fix` comment in the source.

## Known issue

- The first letter of some text strings can look wrong.

## Base

Based on shadPS4 commit `e0a466753a1aacbc8db8253be685bf7ab4d9daae`.

## License

shadPS4 is licensed under GPL-2.0-or-later, and so is this modification.
See [LICENSE](LICENSE). The full source of every released build is in this branch.

No game files, PS4 firmware, system modules, fonts or keys are included or distributed.
You must dump them from your own console and your own copy of the game.
