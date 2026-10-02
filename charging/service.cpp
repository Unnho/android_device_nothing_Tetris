/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 *
 * chargingd: persistent charging control daemon for the CMF Phone 1.
 */

#include <unistd.h>

#include <android-base/logging.h>

#include "ChargingController.h"

int main(int /*argc*/, char** argv) {
    android::base::InitLogging(argv);

    LOG(INFO) << "chargingd starting";

    charging::ChargingController controller;
    while (true) {
        controller.Tick();
        sleep(1);
    }

    return 0;
}
