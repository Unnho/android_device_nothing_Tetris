/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 *
 * Charging control core for the Nothing CMF Phone 1 (Tetris).
 *
 * The daemon owns a small control plane on top of the Nothing/MTK kernel
 * charger nodes:
 *   - /proc/charger/scenario_fcc  (mA FCC voter, controls effective watts)
 *   - /proc/charger/usb_charger_en (charge on/off, shared with LineageOS
 *                                   ChargingControl)
 *   - /proc/charger/real_soc, /proc/charger/usb_temp (state)
 *   - /sys/class/power_supply/mtk-master-charger/voltage_max (VBUS cap)
 *
 * Everything is configured through persist.vendor.charging.* properties so
 * that chargingctl (and plain setprop) can drive it.
 */

#pragma once

#include <string>

namespace charging {

// Persisted configuration properties.
extern const char kPropProfile[];
extern const char kPropSocLimit[];
extern const char kPropSocResume[];
extern const char kPropTempLimitMC[];
extern const char kPropTempStopMC[];
extern const char kPropSuperfastMinutes[];
extern const char kPropSuperfastDeadline[];
extern const char kPropSuperfastPrev[];
extern const char kPropFccSlowMa[];
extern const char kPropFccFastMa[];
extern const char kPropFccSuperfastMa[];
extern const char kPropVoltageMode[];
extern const char kPropEnabled[];

// Runtime status property, published by the daemon (read by chargingctl).
extern const char kPropState[];

// Profile names.
extern const char kProfileSlow[];
extern const char kProfileFast[];
extern const char kProfileSuperfast[];

struct Config {
    std::string profile = kProfileFast;
    int socLimit = 0;            // 0 = disabled, otherwise SOC ceiling in %
    int socResume = 0;           // <= 0 = automatic (socLimit - hysteresis)
    int tempLimitMC = 40000;     // gate: above this, drop to the slow profile
    int tempStopMC = 50000;      // hard stop: stop charging above this
    int superfastMinutes = 30;   // super-fast time box, 0 = no auto revert
    long long superfastDeadline = 0;  // epoch seconds, 0 = no window open
    std::string superfastPrev = kProfileFast;
    int fccSlowMa = 1500;
    int fccFastMa = 3500;
    int fccSuperfastMa = 7500;
    std::string voltageMode = "cap";  // off | cap | exact
    bool enabled = true;
};

class ChargingController {
  public:
    ChargingController();

    // One control cycle; called once per second.
    void Tick();

  private:
    void LoadConfig();
    void HandleSuperfastWindow();
    void ReadSensors();
    std::string EffectiveProfile() const;
    void ApplyFcc(const std::string& profile);
    void ApplyVoltage(const std::string& profile);
    void UpdateHolds();
    void PublishState(const std::string& profile);

    Config cfg_;

    // Sensor snapshot of the current tick (-1 = unavailable).
    int soc_ = -1;
    int batteryTempMC_ = -1;
    int usbTempMC_ = -1;
    int tempMC_ = -1;
    std::string usbType_;

    // Charge on/off holds. Only charging that we disabled ourselves is
    // re-enabled, so LineageOS ChargingControl keeps working unchanged.
    bool socHold_ = false;
    bool tempHold_ = false;
    bool disabledByUs_ = false;

    // Idempotent writes: remember what the kernel accepted last.
    int fccTarget_ = -1;
    int fccWritten_ = -1;
    // Voltage targets are tracked in whatever unit the kernel uses.
    long long voltageTargetUv_ = -1;
    long long voltageWritten_ = -1;
    long long voltageOurLimit_ = -1;
    bool voltageUsable_ = true;

    std::string lastState_;
    std::string lastProfile_;
};

// Helpers shared with chargingctl semantics.
std::string NormalizeProfile(const std::string& value);
std::string NormalizeVoltageMode(const std::string& value);

}  // namespace charging
