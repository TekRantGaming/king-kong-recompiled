// Discord status (Rich Presence; kk_discord_status, on until the player switches it off).
//
// While it's on and the Discord app is running on this PC, the player's Discord profile shows
// "Playing" the port's Discord application, with what they're doing (in the launcher or in the
// game), for how long, the port's version, and buttons to the port's website and Discord server.
// It talks only to the Discord app on this PC, through its local connection (the named pipe
// \\.\pipe\discord-ipc-N, or the socket discord-ipc-N on Linux); nothing is sent anywhere else.
// Without Discord running it tries again every 20 seconds; switched off, the status is cleared.

#pragma once

namespace kk::discord {

// Starts the thread, once; it waits while the setting is off.
void Start();
// The game is running (true) or the launcher is open (false).
void SetPlaying(bool playing);
// The setting changed: shown or cleared at once.
void Wake();
// The port has a Discord application to show (the setting does nothing without one).
bool Available();

}  // namespace kk::discord
