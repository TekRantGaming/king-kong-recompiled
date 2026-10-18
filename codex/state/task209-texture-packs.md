# Task 209 (texpacks-b9): Workshop one page, texture packs, imports, launcher flicker (18 Oct 2026)

## Flicker (owner's Beta 9 test) - FIXED (1824ebe)
- Cause: task 173 heartbeat (launcher.cpp OnHeartbeat) asked the presenter to reconnect (= new swapchain = a
  flash) after 0.4 s without frames, also when the UI thread was only BUSY (slow disk in a 5 GB import, a
  settings write, a long frame). Its posted call then ran right after the busy moment and remade the swapchain.
- Proof: kk\out\task209\run.ps1 -Stall (stall.ps1 suspends only the launcher's UI thread 1.2 s x6): Beta 9
  release build 6 new swapchains; fixed kk-beta 0. Import run before: 5 extra swapchains; after: 0.
- Fix: reconnect only after two heartbeats in a row that ran at once (call waited < 0.15 s). Minimise/restore
  still recovers (restore.ps1 + steps-min.txt). Dev step stall:<ms>.
- Not proven: that this was all of the owner's flicker (no owner log). Swapchain lines in his log
  ("VulkanPresenter: Created ... swapchain" many times) would confirm.

## Hang ("crash") after the import - not diagnosed (owner: don't)
- Owner evidence: AppHangTransient, no crash dump; installed.txt showed the import replaced the HD pack.
- UI thread after an import now: events only; the mods rescan reads pack.txt on its worker; OnDraw worst
  ~2 ms after a 5.3 GB import in tests.

## Workshop = one page, three rows (a36bcab)
- workshop.cpp DrawPage/DrawRow: rows Texture packs, Mods, Other TRG projects and games; per-row focus,
  Up/Down rows, Left/Right (LB/RB) cards, Back returns to the card. Menu of three removed (launcher.cpp).
- workshop_texpacks.{h,cpp} TexturePackShelf: cards original / hd / uhd / mod:<id> / folder / import; page
  with Enable, Disable, Download (UHD asks the owner's note first), Show progress, Remove (confirm), credits.
- KongMods (workshop_mods.*): Cards(), OpenMod, ReloadEnabled, Rescan, IsTexturePack; its Toggle applies
  the rule. KongMods::DrawRow is now unused (left in place).
- Mods row ends with Import mod, Modding guide, Open mods folder, Build your own (kong.tekrantgaming.com/mods).

## One texture pack on (texture_packs_logic.h, tests/texture_packs_tests.cpp)
- Folder packs: native_texture_pack + native_texture_pack_folder (HD pack, own folder); own folder remembered
  in kk_texture_pack_own_folder (mods.cpp). Texture pack mods: kk_mods. EnableFolder/EnableMod/DisableAll.
- Normalize (several on): keep the texture pack mod last in kk_mods (it won before), else the folder pack.
  Applied at launcher start (after the first mods scan) and at game start (mods.cpp ApplyAtGameStart).
- Settings > Graphics: choice row (installed packs) + "Texture packs in the Workshop" + folder row.
  Pause menu: unchanged on/off switch.

## Content service (content.h/.cpp, content_logic.h, tests/content_tests.cpp)
- Kind::kUhd (download QBGh8N8b, 2,423,136,757 B, SHA 72419cbe..., installs into mods\Kong UHD Textures,
  record in content\uhd-textures), Kind::kImport (status of the running import or mod removal).
- Import(zip, target): CheckImportNames before unpack, CheckImportType after; staging in
  content\downloads\import-staging; into mods\<folder>; same mod name = replaced; other = "<name> 2".
  Events via Change names "@texture-pack-mod", "@mod-installed", "@mod-removed". Cancel works for imports.
- Setup pages 6/7: "Already installed" + Continue instead of the silent skip.

## Tools: kk\out\task209\ run.ps1 (-Build, -StepsFile, -FakeHd, -OwnerState, -Uhd, -Zips, -Fresh, -Stall,
  -Restore, -BurstAt), burst.py, make_zips.py, steps-*.txt. Dev steps: workshop:row:/card:/cards/tex:<state|
  open:KEY|enable:KEY|button:N>, content:choose:/choose-mod:/download-uhd/cancel-uhd/cancel-import, stall:MS.

## Not done / open
- Linux compile of the new files not run. Mouse-only and real controller passes not run (dev steps only).
- UHD full download through the launcher never run (link start+cancel only; import of the same ZIP done).
