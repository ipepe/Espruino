/*
 * This file is part of Espruino, a JavaScript interpreter for Microcontrollers
 *
 * Copyright (C) 2019 Gordon Williams <gw@pur3.co.uk>
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * ----------------------------------------------------------------------------
 * Contains JavaScript interface for Bangle.js (http://www.espruino.com/Bangle.js)
 * ----------------------------------------------------------------------------
 */
#include "jspin.h"

void jswrap_banglejs_lcdWr(JsVarInt cmd, JsVar *data);
void jswrap_banglejs_setLCDPower(bool isOn);
void jswrap_banglejs_setLCDMode(JsVar *mode);
void jswrap_banglejs_setLCDTimeout(JsVarFloat timeout);
void jswrap_banglejs_setPollInterval(JsVarFloat interval);
void jswrap_banglejs_setGestureOptions(JsVar *options);
bool jswrap_banglejs_isLCDOn();
bool jswrap_banglejs_isCharging();

void jswrap_banglejs_setGPSPower(bool isOn);
void jswrap_banglejs_setCompassPower(bool isOn);

JsVar *jswrap_banglejs_dbg();
void jswrap_banglejs_accelWr(JsVarInt reg, JsVarInt data);
int jswrap_banglejs_accelRd(JsVarInt reg);
void jswrap_banglejs_compassWr(JsVarInt reg, JsVarInt data);
void jswrap_banglejs_ioWr(JsVarInt mask, bool on);
JsVar *jswrap_banglejs_project(JsVar *latlong);
JsVar *jswrap_banglejs_beep(int time, int freq);
JsVar *jswrap_banglejs_buzz(int time, JsVarFloat amt);
void jswrap_banglejs_off();
JsVar *jswrap_banglejs_menu(JsVar *menu);

void jswrap_banglejs_init();
void jswrap_banglejs_kill();
bool jswrap_banglejs_idle();
bool jswrap_banglejs_gps_character(char ch);
