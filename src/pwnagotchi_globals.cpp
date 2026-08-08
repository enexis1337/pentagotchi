#include "pwnagotchi_internal.h"

namespace pwnagotchi::detail {

portMUX_TYPE gRadioMux = portMUX_INITIALIZER_UNLOCKED;
PwnagotchiApp *gInstance = nullptr;
std::set<BeaconEntry> gRegisteredBeacons;
std::vector<PwngridPeer> gPeers;
uint8_t gTotalFriends = 0;
String gLastFriendName;
int gClosestRssi = -1000;
int gHandshakeCount = 0;

} // namespace pwnagotchi::detail
