# Charging control (chargingd + chargingctl)

Property-driven charging control for the CMF Phone 1 ("Tetris"), layered
on top of the Nothing kernel charger nodes and kept compatible with the
LineageOS ChargingControl feature (which drives
`/proc/charger/usb_charger_en` through
`lineage_health/charging_control_charging_path` in `device.mk`).

## Components

| File | Purpose |
| --- | --- |
| `ChargingController.{h,cpp}` | All the logic: profiles, holds, hysteresis, super-fast windows |
| `service.cpp`, `chargingd.rc` | 1 s poll loop; `class latestart`, runs as `system` |
| `chargingctl.c` | CLI, plain bionic C (see "Why no libbase" below) |
| `Android.bp`, `device.mk` | Build wiring (`PRODUCT_PACKAGES += chargingd chargingctl`) |
| `../sepolicy/vendor/charging_control.te` (+ file/property contexts) | Domains, node and property access |

Every second `chargingd` re-reads `persist.vendor.charging.*`, then:

1. resolves the effective profile (temperature gate, master switch),
2. votes the FCC (`/proc/charger/scenario_fcc`, mA),
3. optionally caps VBUS (`/sys/class/power_supply/mtk-master-charger/voltage_max`),
4. applies the SOC ceiling / hard temperature stop on
   `/proc/charger/usb_charger_en`,
5. publishes a compact snapshot to `vendor.charging.state`.

Because everything is re-applied every tick, a charger replug event that
resets the kernel's votes cannot lose the configuration.

## Wattage profiles

`persist.vendor.charging.profile` = `slow` | `fast` (default) | `superfast`

| Profile | FCC vote | VBUS target | Notes |
| --- | --- | --- | --- |
| `slow` | 1500 mA | 5 V | Travel/overnight profile |
| `fast` | 3500 mA | 9 V | Default; ~18 W class |
| `superfast` | 7500 mA | 11 V | Full 33 W PPS class, time-boxed |

**Stock evidence.** Nothing ships no readable FCC numbers in the image:
the stock `charge-service`/`nt-services` blobs only call
`setChargeFccInt`, toggle `persist.vendor.charge.mode` and log
"use FCC charge" / "use SLOW charge". The unit *is* milliamps, though —
the `nt_chg.ko` driver logs `scenario_fcc` as
`%s: fcc %d ma`, and `/proc/charger/scenario_fcc` is a 0666 mA voter.
The panel is a 33 W device (11 V / 3 A PPS), so `superfast` tops out at
7500 mA / 11 V with `kMaxFccMa = 8000` as a hard safety rail. Each FCC
value can be overridden individually (`fcc_*_ma` props) without
rebuilding.

The VBUS targets are only *applied* according to
`persist.vendor.charging.voltage_mode`:

* `cap` (default) — only ever lowers `voltage_max`, and only raises it
  back to a value we lowered ourselves (never above what the adapter and
  kernel negotiated);
* `exact` — always writes the profile target;
* `off` — never touches the node.

The unit of `voltage_max` is inferred from its current raw value (µV vs
mV) before writing; anything ambiguous disables voltage control for the
boot instead of writing a wrong scale.

## Properties

| Property | Default | Meaning |
| --- | --- | --- |
| `persist.vendor.charging.profile` | `fast` | Requested profile |
| `persist.vendor.charging.fcc_slow_ma` / `fcc_fast_ma` / `fcc_superfast_ma` | 1500 / 3500 / 7500 | FCC votes (mA, clamped 100–8000) |
| `persist.vendor.charging.voltage_mode` | `cap` | `cap` \| `exact` \| `off` |
| `persist.vendor.charging.soc_limit` | 0 | Stop charging at this SOC % (0 = off) |
| `persist.vendor.charging.soc_resume` | 0 | Resume floor % (default: limit − 5) |
| `persist.vendor.charging.temp_limit_mC` | 40000 | Soft gate: profiles drop to `slow` |
| `persist.vendor.charging.temp_stop_mC` | 50000 | Hard stop: charging disabled (min limit + 5000) |
| `persist.vendor.charging.superfast_minutes` | 30 | Super-fast window length (0 = until changed) |
| `persist.vendor.charging.superfast_deadline` / `superfast_prev` | 0 / `fast` | Daemon bookkeeping for the window |
| `persist.vendor.charging.enabled` | 1 | Master switch (0 = daemon touches nothing) |
| `vendor.charging.state` | — | Read-only snapshot, see below |

Temperatures are millidegrees Celsius; both the gauge (power_supply
decidegrees) and `/proc/charger/usb_temp` are normalized, and the hotter
of the two gates the daemon.

`vendor.charging.state` is a compact `key=value;...` string, kept under
92 bytes because init rejects longer property values:

```
profile=fast;eff=slow;fcc=1500;soc=72;temp=41000;hold=none;en=1;volt=cap
```

(`eff` = effective profile after the temperature gate, `hold` = `soc` /
`temp` / `none`, `en` = raw `usb_charger_en`, `usb` appended when known.)

## Holds never fight LineageOS ChargingControl

When a hold triggers, the daemon writes `0` to `usb_charger_en` **only if
the node is currently `1`**. If charging is already off, it means the user
or LineageOS ChargingControl turned it off — the daemon does not claim it
and will never turn it back on. It only ever re-enables charging if it
disabled it itself (`disabledByUs_`).

## Usage

```
chargingctl status                      # config + live state
chargingctl profile superfast 45        # 45 min window, then auto-revert
chargingctl soc 80                      # stop at 80 %, resume ~75 %
chargingctl temp 40000 50000            # soft gate / hard stop
chargingctl timeout 30                  # window length, 0 = unlimited
chargingctl enable 0                    # pause all control
chargingctl reset                       # defaults
```

## Why chargingctl is plain bionic C

`shell` is a coredomain: AOSP's neverallows let it *execute* vendor
binaries but forbid it from `open`-ing files labelled `vendor_file`
(generic `/vendor/lib64` libraries). A libbase/libc++ tool would die on
the first library load from `adb shell`, so `chargingctl` only uses
`__system_property_get/set` from bionic. It sets properties only — it
never opens the kernel nodes, so it cannot race the daemon.

## sepolicy

* `vendor_charging_control` (init-laemon domain) gets `rw` on
  `vendor_proc_charger` (`/proc/charger`, existing genfscon entry shared
  with `hal_lineage_health_default`) and `rw` on `sysfs_batteryinfo`,
  under which MTK's base `genfs_contexts` already labels
  `/devices/platform/charger/power_supply` — that is where
  `mtk-master-charger/voltage_max` lives — plus `set_prop`/`get_prop` on
  the new `vendor_charging_control_prop`.
* The property type is `vendor_public_prop`, the documented
  coredomain-settable class, so `shell`'s `set_prop` grant survives the
  `coredomain`/`vendor_property_type` neverallows.

## Validation and risks

Host build checks: `ChargingController.cpp` + `service.cpp` compile with
`-std=gnu++17 -Wall -Werror`, `chargingctl.c` with `-std=gnu11
-Wall -Werror`, and a 23-case faked-backend test suite covers profile
votes, temperature gating, both hysteresis loops, external-disable
protection, super-fast windows/revert, voltage-cap safety, the master
switch and the state-property size limit.

Known limits:

* Stock revealed no numeric FCC values, so the profile mA numbers are
  engineering defaults derived from the 33 W envelope — tune via props.
* `voltage_max` control is best-effort: an unexpected node unit disables
  it for the boot rather than risking a wrong write.
* There is no hardware-level guarantee behind `kMaxFccMa`; thermal and
  charger-voter clamps in the kernel remain the real safety net.
