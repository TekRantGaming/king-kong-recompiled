// Online player count (kk_online_count, on until the player switches it off).
//
// While it's on, a background thread checks in with the port's server every 5
// minutes (and at once when it's switched on or the game starts), and gets back
// how many launchers checked in within the last 10 minutes and how many of them
// are in the game. A check-in holds a random install ID (kk_install_id, made
// when it's switched on and deleted when it's switched off), the port's version,
// the newest version it was offered, the system (win32, or linux for Linux and
// Proton), x64, the update channel and whether the game is running. Nothing is
// sent while it's off. Beta 7: the same switch also covers the pipeline file,
// performance summary and crash report uploads (telemetry.h).

#pragma once

#include <string>

namespace kk::online {

// Starts the thread, once; it waits while the setting is off.
void Start();
// Sends a check-in now (the setting was just switched on).
void Wake();
// The game is running: said in the check-ins from now on (one goes now).
void SetPlaying(bool playing);
// The newest version the update check found.
void SetAvailableVersion(const std::string& version);
// A new random install ID ("xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx").
std::string NewInstallId();
// This install's ID (kk_install_id), made now when the switch is on and there is none yet; empty while off.
std::string InstallId();
// The server (developer builds: KK_DEV_ONLINE_URL when set).
std::string ServerUrl();

struct Counts {
  bool known = false;  // the server has answered since the setting was switched on
  int online = 0, playing = 0;
};
Counts Get();

}  // namespace kk::online
