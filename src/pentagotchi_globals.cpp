#include "pentagotchi_internal.h"

bool gSerialEnabled = false;

namespace pentagotchi::detail {

portMUX_TYPE gRadioMux = portMUX_INITIALIZER_UNLOCKED;
PentagotchiApp *gInstance = nullptr;
SemaphoreHandle_t gPeersMutex = nullptr;
std::set<BeaconEntry> gRegisteredBeacons;
std::set<uint64_t> gHandshakeBssids;
std::vector<PwngridPeer> gPeers;
uint8_t gTotalFriends = 0;
String gLastFriendName;
String gLastPwndName;
String gLastHandshakeFile;
uint8_t gLastHandshakeMac[6] = {0};
bool gLastHandshakeMacValid = false;
int gClosestRssi = -1000;
int gHandshakeCount = 0;

} // namespace pentagotchi::detail
