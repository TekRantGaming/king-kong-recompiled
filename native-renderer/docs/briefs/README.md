# Phase 1 agent briefs

Each brief is self-contained: an agent starting cold should be able to work from it plus the two design
documents. Common rules for every workstream:

- Work only inside `F:\KK-native-renderer` (your own worktree of `kk-recomp`, branch `native-renderer/<stream>`),
  never in `C:\Peter Jackson's King Kong Recomp` or `C:\rexsrc`. Commit on your branch; don't merge.
- Read `docs/design.md` and `docs/d3d-api-map.md` first. Addresses are the game's (Xbox 360) addresses;
  `sub_XXXXXXXX` names are the recompiled functions in `kk/generated/default` (big-endian guest data; `base +
  address` is host memory in hooks).
- Never commit game data, anything derived from the game's files (dumps, extracted shaders or textures), the
  SDK binaries or the translated C++ (`kk/generated/default` is already in git; `analysis/` outputs are not).
- The game may only be run when no other session is running it (check for a `king_kong` process first); the
  dev build is `kk/out/build/kk-dev/king_kong.exe` (`kk\build.bat kk-dev`), run with
  `--game_data_root=F:/KK-native-renderer/kk-recomp/kk/assets --user_data_root=F:/KK-native-renderer/userdata
  --cache_root=F:/KK-native-renderer/cache --kk_launcher=false`. `KK_AUTOPLAY=1` presses Play;
  `KK_DEV_AUTOSKIP=1` with `KK_DEV_SCRIPT` plays into a chapter (see `kk/src/dev_tools.cpp`); the V-Rex script
  is in `analysis/` run commands. Never leave the game running.
- No downloads without the user's permission (state file, source and size). NVRHI and XenosRecomp are
  pre-approved once the user has said so in the brief's "inputs".
- Plain language in docs and commit messages; no em dashes. Commits end with the attribution line the host
  gives you.

| Brief | Stream | Checked against |
|---|---|---|
| `01-d3d-layer-map.md` | every entry point and render state named, with argument structs | the traces, the register table |
| `02-shader-translator.md` | all shaders translated and compiling on D3D12 and Vulkan | the disc's HLSL sources, today's renderer |
| `03-textures-resources.md` | guest texture / buffer formats to host uploads | today's texture cache |
| `04-backend-plugin.md` | the `rexgpu-native` plugin skeleton on NVRHI, presenting through the SDK | a clear colour on screen, then a triangle |
| `05-test-harness.md` | golden frames, draw logs and a comparison tool | n/a |
