/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ChargingController.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/strings.h>

#include <algorithm>
#include <cstdlib>
#include <sstream>

namespace charging {

const char kPropProfile[] = "persist.vendor.charging.profile";
const char kPropSocLimit[] = "persist.vendor.charging.soc_limit";
const char kPropSocResume[] = "persist.vendor.charging.soc_resume";
const char kPropTempLimitMC[] = "persist.vendor.charging.temp_limit_mC";
const char kPropTempStopMC[] = "persist.vendor.charging.temp_stop_mC";
const char kPropSuperfastMinutes[] = "persist.vendor.charging.superfast_minutes";
const char kPropSuperfastDeadline[] = "persist.vendor.charging.superfast_deadline";
const char kPropSuperfastPrev[] = "persist.vendor.charging.superfast_prev";
const char kPropFccSlowMa[] = "persist.vendor.charging.fcc_slow_ma";
const char kPropFccFastMa[] = "persist.vendor.charging.fcc_fast_ma";
const char kPropFccSuperfastMa[] = "persist.vendor.charging.fcc_superfast_ma";
const char kPropVoltageMode[] = "persist.vendor.charging.voltage_mode";
const char kPropEnabled[] = "persist.vendor.charging.enabled";
const char kPropState[] = "vendor.charging.state";

const char kProfileSlow[] = "slow";
const char kProfileFast[] = "fast";
const char kProfileSuperfast[] = "superfast";

namespace {

// Kernel control/state nodes (chmod'ed by init.mt6878.rc).
constexpr char kPathFcc[] = "/proc/charger/scenario_fcc";
constexpr char kPathChargerEn[] = "/proc/charger/usb_charger_en";
constexpr char kPathRealSoc[] = "/proc/charger/real_soc";
constexpr char kPathUsbTemp[] = "/proc/charger/usb_temp";
constexpr char kPathUsbType[] = "/proc/charger/usb_real_type";
constexpr char kPathBatteryCapacity[] = "/sys/class/power_supply/battery/capacity";
constexpr char kPathBatteryTemp[] = "/sys/class/power_supply/battery/temp";
constexpr char kPathVoltageMax[] = "/sys/class/power_supply/mtk-master-charger/voltage_max";

// Safety rails: never vote more than the hardware budget, keep the FCC sane.
constexpr int kMinFccMa = 100;
constexpr int kMaxFccMa = 8000;
// Hard stop is released this many millidegrees below its threshold.
constexpr int kTempResumeDeltaMC = 5000;
// Default SOC ceiling hysteresis in percent.
constexpr int kSocHysteresisPct = 5;
// Profile VBUS targets. CMF Phone 1 ships 33 W (11 V / 3 A PPS).
constexpr long long kVoltageSlowUv = 5000000LL;
constexpr long long kVoltageFastUv = 9000000LL;
constexpr long long kVoltageSuperfastUv = 11000000LL;

std::string GetProp(const char* key, const std::string& fallback) {
    std::string value = android::base::GetProperty(key, "");
    return value.empty() ? fallback : value;
}

int GetIntProp(const char* key, int fallback) {
    std::string value = android::base::GetProperty(key, "");
    if (value.empty()) return fallback;
    char* end = nullptr;
    long parsed = strtol(value.c_str(), &end, 10);
    if (end == value.c_str()) return fallback;
    return static_cast<int>(parsed);
}

long long GetLongProp(const char* key, long long fallback) {
    std::string value = android::base::GetProperty(key, "");
    if (value.empty()) return fallback;
    char* end = nullptr;
    long long parsed = strtoll(value.c_str(), &end, 10);
    if (end == value.c_str()) return fallback;
    return parsed;
}

int ReadIntFile(const char* path, int fallback) {
    std::string raw;
    if (!android::base::ReadFileToString(path, &raw)) return fallback;
    raw = android::base::Trim(raw);
    if (raw.empty()) return fallback;
    char* end = nullptr;
    long parsed = strtol(raw.c_str(), &end, 10);
    if (end == raw.c_str()) return fallback;
    return static_cast<int>(parsed);
}

long long ReadLongFile(const char* path, long long fallback) {
    std::string raw;
    if (!android::base::ReadFileToString(path, &raw)) return fallback;
    raw = android::base::Trim(raw);
    if (raw.empty()) return fallback;
    char* end = nullptr;
    long long parsed = strtoll(raw.c_str(), &end, 10);
    if (end == raw.c_str()) return fallback;
    return parsed;
}

std::string ReadStringFile(const char* path) {
    std::string raw;
    if (!android::base::ReadFileToString(path, &raw)) return "";
    return android::base::Trim(raw);
}

bool WriteFile(const char* path, const std::string& value) {
    if (!android::base::WriteStringToFile(value, path)) {
        PLOG(ERROR) << "failed to write '" << value << "' to " << path;
        return false;
    }
    return true;
}

// Both the battery gauge (power_supply ABI, decidegrees) and
// /proc/charger/usb_temp are handled here: anything that is small enough to
// be decidegrees Celsius is scaled to millidegrees, anything else is taken
// as millidegrees already.
int NormalizeTempMC(int raw) {
    int magnitude = raw < 0 ? -raw : raw;
    if (magnitude <= 1000) return raw * 100;
    return raw;
}

int Clamp(int value, int low, int high) {
    return std::min(std::max(value, low), high);
}

// Unit of /sys/class/power_supply/mtk-master-charger/voltage_max is not
// verifiable from the tree (power_supply ABI says microvolts, some kernels
// use millivolts), so infer it from what the node currently reports.
// Returns the divisor for our microvolt targets, or 0 when ambiguous.
long long VoltageDivisor(long long raw) {
    if (raw >= 4000000LL && raw <= 20000000LL) return 1;      // microvolts
    if (raw >= 4000LL && raw <= 20000LL) return 1000;          // millivolts
    return 0;
}

long long VoltageTargetUv(const std::string& profile) {
    if (profile == kProfileSlow) return kVoltageSlowUv;
    if (profile == kProfileSuperfast) return kVoltageSuperfastUv;
    return kVoltageFastUv;
}

}  // namespace

std::string NormalizeProfile(const std::string& value) {
    std::string profile = android::base::ToLower(value);
    if (profile == kProfileSlow || profile == kProfileSuperfast) return profile;
    if (profile != kProfileFast) {
        LOG(WARNING) << "unknown charging profile '" << value << "', using " << kProfileFast;
        return kProfileFast;
    }
    return profile;
}

std::string NormalizeVoltageMode(const std::string& value) {
    std::string mode = android::base::ToLower(value);
    if (mode == "off" || mode == "exact") return mode;
    if (mode != "cap") {
        LOG(WARNING) << "unknown voltage mode '" << value << "', using cap";
        return "cap";
    }
    return mode;
}

ChargingController::ChargingController() {
    LoadConfig();
    lastProfile_ = cfg_.profile;
    LOG(INFO) << "charging control up: profile=" << cfg_.profile
              << " soc_limit=" << cfg_.socLimit
              << " temp_limit_mC=" << cfg_.tempLimitMC
              << " temp_stop_mC=" << cfg_.tempStopMC
              << " superfast_minutes=" << cfg_.superfastMinutes;
}

void ChargingController::Tick() {
    LoadConfig();
    HandleSuperfastWindow();
    ReadSensors();

    const std::string profile = EffectiveProfile();
    if (profile != lastProfile_) {
        LOG(INFO) << "effective profile " << lastProfile_ << " -> " << profile
                  << " (requested " << cfg_.profile << ", temp_mC=" << tempMC_ << ")";
        lastProfile_ = profile;
    }

    ApplyFcc(profile);
    ApplyVoltage(profile);
    UpdateHolds();
    PublishState(profile);
}

void ChargingController::LoadConfig() {
    cfg_.profile = NormalizeProfile(GetProp(kPropProfile, kProfileFast));
    cfg_.socLimit = Clamp(GetIntProp(kPropSocLimit, 0), 0, 100);
    cfg_.socResume = Clamp(GetIntProp(kPropSocResume, 0), 0, 100);
    cfg_.tempLimitMC = GetIntProp(kPropTempLimitMC, 40000);
    cfg_.tempStopMC = GetIntProp(kPropTempStopMC, 50000);
    // Keep the hard stop above the soft gate so the states cannot flip-flop.
    if (cfg_.tempStopMC <= cfg_.tempLimitMC) cfg_.tempStopMC = cfg_.tempLimitMC + kTempResumeDeltaMC;
    cfg_.superfastMinutes = Clamp(GetIntProp(kPropSuperfastMinutes, 30), 0, 24 * 60);
    cfg_.superfastDeadline = GetLongProp(kPropSuperfastDeadline, 0);
    cfg_.superfastPrev = NormalizeProfile(GetProp(kPropSuperfastPrev, kProfileFast));
    cfg_.fccSlowMa = Clamp(GetIntProp(kPropFccSlowMa, 1500), kMinFccMa, kMaxFccMa);
    cfg_.fccFastMa = Clamp(GetIntProp(kPropFccFastMa, 3500), kMinFccMa, kMaxFccMa);
    cfg_.fccSuperfastMa = Clamp(GetIntProp(kPropFccSuperfastMa, 7500), kMinFccMa, kMaxFccMa);
    cfg_.voltageMode = NormalizeVoltageMode(GetProp(kPropVoltageMode, "cap"));
    cfg_.enabled = GetIntProp(kPropEnabled, 1) != 0;
}

void ChargingController::HandleSuperfastWindow() {
    const long long now = static_cast<long long>(time(nullptr));

    if (cfg_.profile == kProfileSuperfast) {
        if (cfg_.superfastMinutes > 0 && cfg_.superfastDeadline == 0) {
            // New window: remember how long and fall back to the profile that
            // was selected before super-fast was requested.
            long long deadline = now + static_cast<long long>(cfg_.superfastMinutes) * 60;
            if (android::base::SetProperty(kPropSuperfastDeadline, std::to_string(deadline))) {
                cfg_.superfastDeadline = deadline;
                LOG(INFO) << "super-fast window opened for " << cfg_.superfastMinutes
                          << " minutes (revert to " << cfg_.superfastPrev << ")";
            } else {
                LOG(ERROR) << "unable to persist the super-fast deadline";
            }
        } else if (cfg_.superfastDeadline != 0 && now >= cfg_.superfastDeadline) {
            const std::string revert = cfg_.superfastPrev;
            android::base::SetProperty(kPropProfile, revert);
            android::base::SetProperty(kPropSuperfastDeadline, "0");
            cfg_.profile = revert;
            cfg_.superfastDeadline = 0;
            LOG(INFO) << "super-fast window expired, profile reverted to " << revert;
        }
        return;
    }

    if (cfg_.superfastDeadline != 0) {
        android::base::SetProperty(kPropSuperfastDeadline, "0");
        cfg_.superfastDeadline = 0;
    }
    // Track the last non-superfast profile: it is where super-fast reverts to.
    if (cfg_.profile != cfg_.superfastPrev) {
        android::base::SetProperty(kPropSuperfastPrev, cfg_.profile);
        cfg_.superfastPrev = cfg_.profile;
    }
}

void ChargingController::ReadSensors() {
    soc_ = ReadIntFile(kPathRealSoc, -1);
    if (soc_ < 0 || soc_ > 100) soc_ = ReadIntFile(kPathBatteryCapacity, -1);
    if (soc_ > 100) soc_ = -1;

    batteryTempMC_ = ReadIntFile(kPathBatteryTemp, -1);
    if (batteryTempMC_ != -1) batteryTempMC_ = NormalizeTempMC(batteryTempMC_);

    usbTempMC_ = ReadIntFile(kPathUsbTemp, -1);
    if (usbTempMC_ != -1) usbTempMC_ = NormalizeTempMC(usbTempMC_);

    // Gate on the hottest sensor we can actually read.
    tempMC_ = std::max(batteryTempMC_, usbTempMC_);
    usbType_ = ReadStringFile(kPathUsbType);
}

std::string ChargingController::EffectiveProfile() const {
    if (!cfg_.enabled) return cfg_.profile;
    // Never raise the charge rate when the battery or the USB port is hot.
    if (tempMC_ >= 0 && tempMC_ >= cfg_.tempLimitMC) return kProfileSlow;
    return cfg_.profile;
}

void ChargingController::ApplyFcc(const std::string& profile) {
    if (!cfg_.enabled) return;

    int target = cfg_.fccFastMa;
    if (profile == kProfileSlow) target = cfg_.fccSlowMa;
    if (profile == kProfileSuperfast) target = cfg_.fccSuperfastMa;
    target = Clamp(target, kMinFccMa, kMaxFccMa);

    const int current = ReadIntFile(kPathFcc, -1);
    const bool targetChanged = (target != fccTarget_);
    const bool outOfSync = (current >= 0 && current != fccWritten_);
    if (!targetChanged && !outOfSync) return;

    if (!WriteFile(kPathFcc, std::to_string(target))) return;

    fccTarget_ = target;
    const int stored = ReadIntFile(kPathFcc, target);
    fccWritten_ = stored >= 0 ? stored : target;
    if (fccWritten_ != target) {
        // The kernel keeps its own voters: a mismatch just means someone else
        // (thermal, health) is clamping harder, which is fine.
        VLOG(1) << "scenario_fcc stored " << fccWritten_ << " mA (voted " << target << " mA)";
    }
}

void ChargingController::ApplyVoltage(const std::string& profile) {
    if (!cfg_.enabled || !voltageUsable_ || cfg_.voltageMode == "off") return;

    const long long raw = ReadLongFile(kPathVoltageMax, -1);
    if (raw < 0) return;  // node absent: nothing to do
    if (raw == 0) return;  // not negotiated yet

    const long long divisor = VoltageDivisor(raw);
    if (divisor == 0) {
        LOG(WARNING) << "unexpected voltage_max value " << raw << ", voltage control disabled";
        voltageUsable_ = false;
        return;
    }

    const long long target = VoltageTargetUv(profile) / divisor;
    const bool targetChanged = (target != voltageTargetUv_);
    const bool outOfSync = (raw != voltageWritten_);
    if (!targetChanged && !outOfSync) return;

    if (raw == target) {
        voltageTargetUv_ = target;
        voltageWritten_ = raw;
        return;
    }

    // Lowering the request is always fine. Raising it is only allowed when
    // the current value is one we wrote ourselves ("cap" mode never pushes
    // the charger above what the adapter/kernel negotiated) or in "exact".
    const bool mayRaise = (cfg_.voltageMode == "exact") || (raw == voltageOurLimit_);
    if (target > raw && !mayRaise) {
        voltageTargetUv_ = target;
        voltageWritten_ = raw;
        return;
    }

    if (!WriteFile(kPathVoltageMax, std::to_string(target))) {
        voltageUsable_ = false;
        return;
    }
    voltageTargetUv_ = target;
    const long long stored = ReadLongFile(kPathVoltageMax, target);
    voltageWritten_ = stored >= 0 ? stored : target;
    voltageOurLimit_ = voltageWritten_;
    VLOG(1) << "voltage_max set to " << voltageWritten_ << " (unit scale " << divisor << ")";
}

void ChargingController::UpdateHolds() {
    if (!cfg_.enabled) return;  // daemon paused: never touch the charge path

    // SOC ceiling with hysteresis.
    if (cfg_.socLimit <= 0) {
        socHold_ = false;
    } else if (soc_ >= 0 && soc_ <= 100) {
        int resume = cfg_.socResume > 0 ? cfg_.socResume : cfg_.socLimit - kSocHysteresisPct;
        resume = Clamp(resume, 1, cfg_.socLimit - 1);
        if (soc_ >= cfg_.socLimit) {
            socHold_ = true;
        } else if (soc_ <= resume) {
            socHold_ = false;
        }
    }

    // Hard temperature stop with hysteresis.
    if (tempMC_ >= 0) {
        if (tempMC_ >= cfg_.tempStopMC) {
            tempHold_ = true;
        } else if (tempMC_ <= cfg_.tempStopMC - kTempResumeDeltaMC) {
            tempHold_ = false;
        }
    }

    if (socHold_ || tempHold_) {
        // Already off means somebody else (LineageOS ChargingControl, the
        // user) turned it off: never claim it, so we never turn it back on.
        if (ReadIntFile(kPathChargerEn, -1) == 0) return;
        if (WriteFile(kPathChargerEn, "0")) {
            disabledByUs_ = true;
            VLOG(1) << "charging held off (soc_hold=" << socHold_ << " temp_hold=" << tempHold_ << ")";
        }
    } else if (disabledByUs_) {
        if (WriteFile(kPathChargerEn, "1")) {
            disabledByUs_ = false;
            VLOG(1) << "charging hold released";
        }
    }
}

void ChargingController::PublishState(const std::string& profile) {
    // Property values must stay strictly below PROP_VALUE_MAX (92 bytes),
    // or init refuses them with "Property value too long".
    constexpr size_t kMaxStateLen = 91;

    std::ostringstream state;
    state << "profile=" << cfg_.profile
          << ";eff=" << profile
          << ";fcc=" << fccTarget_
          << ";soc=" << soc_
          << ";temp=" << tempMC_
          << ";hold=" << (socHold_ ? "soc" : (tempHold_ ? "temp" : "none"))
          << ";en=" << ReadIntFile(kPathChargerEn, -1);
    std::string value = state.str();

    // Optional extras are appended only while the whole value stays legal.
    const auto append_if_fits = [&value, kMaxStateLen](const std::string& field) {
        if (value.size() + field.size() <= kMaxStateLen) value += field;
    };
    append_if_fits(";volt=" + cfg_.voltageMode);
    if (!usbType_.empty()) append_if_fits(";usb=" + usbType_);

    if (value.size() > kMaxStateLen) value.resize(kMaxStateLen);  // belt and braces
    if (value == lastState_) return;
    if (!android::base::SetProperty(kPropState, value)) {
        VLOG(1) << "unable to publish " << kPropState;
    }
    lastState_ = value;
}

}  // namespace charging
