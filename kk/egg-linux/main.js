// King Kong Recompiled, Linux only: starts the owner's Easter egg game (resources/egg.asar, unchanged) and lets
// the launcher pause it (kk/src/easter_egg.cpp). On Wayland the launcher cannot put its own window in front of
// another program's, so the game's window steps aside instead.
//   launcher -> stdin:  "hide" (the pause panel), "show" (Resume), "close" (Quit to main menu)
//   stdout -> launcher: "shown", "focus 1" / "focus 0", "down enter" / "up enter", "down escape" / "up escape"
// The game quits when the launcher goes (stdin ends), as the Windows job ends it.
const {app, BrowserWindow} = require('electron');
const path = require('path');

const egg = path.join(process.resourcesPath, 'egg.asar');
const say = (line) => { try { process.stdout.write(line + '\n'); } catch (e) {} };

app.on('browser-window-created', (event, win) => {
  win.on('show', () => say('shown'));
  win.on('focus', () => say('focus 1'));
  win.on('blur', () => say('focus 0'));
  win.webContents.on('before-input-event', (e, input) => {
    if (input.key === 'Enter' || input.key === 'Escape')
      say((input.type === 'keyDown' ? 'down ' : 'up ') + input.key.toLowerCase());
  });
});

let pending = '';
process.stdin.setEncoding('utf8');
process.stdin.on('data', (data) => {
  pending += data;
  for (let n; (n = pending.indexOf('\n')) >= 0;) {
    const command = pending.slice(0, n).trim();
    pending = pending.slice(n + 1);
    if (command === 'close') app.quit();
    for (const win of BrowserWindow.getAllWindows()) {
      if (command === 'hide') win.hide();
      if (command === 'show') { win.show(); win.setFullScreen(true); win.focus(); }
    }
  }
});
process.stdin.on('end', () => app.quit());

require(path.join(egg, require(path.join(egg, 'package.json')).main));
