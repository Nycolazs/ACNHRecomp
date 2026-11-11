// "Fix Save Linked Account" (settings, Online page): clears the online account an ACNH save was linked to, so a save
// played on another network (Rustendo, Pretendo, a console) loads under Nextendo instead of "linked to a different
// account". The game is restarted around the change: request_restart() relaunches this process with NX_UNLINK_SAVE=1,
// and run_if_requested() makes the change at boot, before the game opens the save. The save as it was is copied first
// to <user>/backups/[save-fixed]/<island> <date time> (never pruned, not in the Saves page's backup list).
#pragma once

#include <string>

namespace nx::save_unlink {

// Unlink the save in `device_save` (main.dat, mainHeader.dat, Villager0..7). Refuses anything it does not recognise
// (not version 3.0.x, a hash that does not match) and changes nothing then. `message` says what happened.
bool unlink(const std::string& device_save, std::string& message);

// Relaunch the game with the fix requested; does not return when it worked (false when the relaunch failed).
bool request_restart();
// At boot: when NX_UNLINK_SAVE=1, unlink the device save (logged; last_result() for the settings screen).
void run_if_requested();
std::string last_result();

}  // namespace nx::save_unlink
