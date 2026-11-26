// UWP battery state for the in-game status bar. The app container has no GetSystemPowerStatus, so this asks
// Windows.System.Power instead. -1 means the device has no battery reading (an Xbox, a desktop).
#include <windows.h>

using namespace Windows::System::Power;

extern "C" int pd_uwp_battery(int *charging) {
    if (charging)
        *charging = 0;
    try {
        const int percent = static_cast<int>(PowerManager::RemainingChargePercent);
        if (charging)
            *charging = PowerManager::BatteryStatus == BatteryStatus::Charging;
        return percent >= 0 && percent <= 100 ? percent : -1;
    } catch (...) {
        return -1;
    }
}