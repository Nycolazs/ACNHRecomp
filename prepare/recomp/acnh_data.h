// ACNH's files the game folder's preparation needs, embedded at build time (recomp.cmake: acnh_data.inc):
// games/acnh/versions.txt and hooks*.txt (which hooks the recompiled main gets per build) and tmouh's SpeedFix for main
// (acnh/speedfix-*.pchtxt, licence in acnh/SPEEDFIX.md). File name -> content.
#pragma once

#include <map>
#include <string>

namespace rc {

const std::map<std::string, std::string>& acnh_data();

}  // namespace rc
