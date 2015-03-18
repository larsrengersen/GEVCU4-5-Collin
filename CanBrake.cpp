/*
 * CanBrake.cpp
 *
Copyright (c) 2013 Collin Kidder, Michael Neuweiler, Charles Galpin

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

 */

#include "CanBrake.h"

CanBrake::CanBrake() : Throttle()
{
    canHandlerCar = CanHandler::getInstanceCar();
    prefsHandler = new PrefHandler(CANBRAKEPEDAL);

    rawSignal.input1 = 0;
    rawSignal.input2 = 0;
    rawSignal.input3 = 0;
    ticksNoResponse = 255; // invalidate input signal until response is received
    responseId = 0;
    responseMask = 0x7ff;
    responseExtended = false;

    commonName = "CANBus brake";
}

void CanBrake::setup()
{
    tickHandler->detach(this);

    loadConfiguration();
    Throttle::setup();

    requestFrame.length = 0x08;
    requestFrame.rtr = 0x00;
    requestFrame.extended = 0x00;

    CanBrakeConfiguration *config = (CanBrakeConfiguration *) getConfiguration();

    switch (config->carType) {
        case Volvo_S80_Gas:
            // Request: dlc=0x8 fid=0x760 id=0x760 ide=0x0 rtr=0x0 data=0x03,0x22,0x2B,0x0D,0x00,0x00,0x00,0x00 (vida: [0x00, 0x00, 0x07, 0x60, 0x22, 0x2B, 0x0D])
            // Response: dlc=0x8 fid=0x768 id=0x768 ide=0x0 rtr=0x0 data=0x05,0x62,0x2B,0x0D,0x00,0x01,0x00,0x00 (vida: [0x00, 0x00, 0x07, 0x68, 0x62, 0x2B, 0x0D, 0x00, 0x01]), 6th byte
            requestFrame.id = 0x760;
            memcpy(requestFrame.data.bytes, (const uint8_t[]) {
                0x03, 0x22, 0x2B, 0x0D, 0x00, 0x00, 0x00, 0x00
            }, 8);
            responseId = 0x768;
            break;

        case Volvo_V50_Diesel:
            // Request: dlc=0x08 fid=0xFFFFE id=0x3FFFE ide=0x01 rtr=0x00 data=0xCD,0x11,0xA6,0x00,0x24,0x01,0x00,0x00 ([0x00, 0xf, 0xff, 0xfe, 0xcd, 0x11, 0xa6, 0x00, 0x24, 0x01, 0x00, 0x00])
            // Response: dlc=0x08 fid=0x400021 id=0x21 ide=0x01 rtr=0x00 data=0xCE,0x11,0xE6,0x00,0x24,0x03,0xFD,0x00 (vida: [0x00, 0x40, 0x00, 0x21, 0xce, 0x11, 0xe6, 0x00, 0x24, 0x03, 0xfd, 0x00])
//      requestFrame.id = 0x3FFFE;
//      requestFrame.ide = 0x01;
//      memcpy(requestFrame.data, (uint8_t[]){ 0xce, 0x11, 0xe6, 0x00, 0x24, 0x03, 0xfd, 0x00 }, 8);
//      responseId = 0x21;
//      responseExtended = true;
            break;

        default:
            Logger::error(CANBRAKEPEDAL, "no valid car type defined.");
    }

    canHandlerCar->attach(this, responseId, responseMask, responseExtended);
    tickHandler->attach(this, CFG_TICK_INTERVAL_CAN_THROTTLE);
}

/*
 * Send a request to the ECU.
 *
 */
void CanBrake::handleTick()
{
    Throttle::handleTick(); // Call parent handleTick

    canHandlerCar->sendFrame(requestFrame);

    if (ticksNoResponse < 255) { // make sure it doesn't overflow
        ticksNoResponse++;
    }
}

/*
 * Handle the response of the ECU and calculate the throttle value
 *
 */
void CanBrake::handleCanFrame(CAN_FRAME *frame)
{
    CanBrakeConfiguration *config = (CanBrakeConfiguration *) getConfiguration();

    if (frame->id == responseId) {
        switch (config->carType) {
            case Volvo_S80_Gas:
                rawSignal.input1 = frame->data.bytes[5];
                break;

            case Volvo_V50_Diesel:
//              rawSignal.input1 = (frame->data.bytes[5] + 1) * frame->data.bytes[6];
                break;
        }

        ticksNoResponse = 0;
    }
}

RawSignalData* CanBrake::acquireRawSignal()
{
    return &rawSignal; // should have already happened in the background
}

bool CanBrake::validateSignal(RawSignalData* rawSignal)
{
    CanBrakeConfiguration *config = (CanBrakeConfiguration *) getConfiguration();

    if (ticksNoResponse >= CFG_CANTHROTTLE_MAX_NUM_LOST_MSG) {
        if (status == OK) {
            Logger::error(CANBRAKEPEDAL, "no response on position request received: %d ", ticksNoResponse);
        }

        status = ERR_MISC;
        return false;
    }

    if (rawSignal->input1 > (config->maximumLevel1 + CFG_THROTTLE_TOLERANCE)) {
        if (status == OK) {
            Logger::error(CANBRAKEPEDAL, (char *) Constants::valueOutOfRange, rawSignal->input1);
        }

        status = ERR_HIGH_T1;
        return false;
    }

    if (rawSignal->input1 < (config->minimumLevel1 - CFG_THROTTLE_TOLERANCE)) {
        if (status == OK) {
            Logger::error(CANBRAKEPEDAL, (char *) Constants::valueOutOfRange, rawSignal->input1);
        }

        status = ERR_LOW_T1;
        return false;
    }

    // all checks passed -> brake is working
    if (status != OK) {
        Logger::info(CANBRAKEPEDAL, (char *) Constants::normalOperation);
    }

    status = OK;
    return true;
}

uint16_t CanBrake::calculatePedalPosition(RawSignalData* rawSignal)
{
    CanBrakeConfiguration *config = (CanBrakeConfiguration *) getConfiguration();

    if (config->maximumLevel1 == 0) { //brake processing disabled if max is 0
        return 0;
    }

    return normalizeAndConstrainInput(rawSignal->input1, config->minimumLevel1, config->maximumLevel1);
}

/*
 * Overrides the standard implementation of throttle mapping as different rules apply to
 * brake based regen.
 */
int16_t CanBrake::mapPedalPosition(int16_t pedalPosition)
{
    CanBrakeConfiguration *config = (CanBrakeConfiguration *) getConfiguration();
    int16_t brakeLevel, range;

    if (pedalPosition == 0) { // if brake not pressed, return 0, not minimumRegen !
        return 0;
    }

    range = config->maximumRegen - config->minimumRegen;
    brakeLevel = -10 * range * pedalPosition / 1000;
    brakeLevel -= 10 * config->minimumRegen;

    return brakeLevel;
}

DeviceId CanBrake::getId()
{
    return CANBRAKEPEDAL;
}

/*
 * Return the device type
 */
DeviceType CanBrake::getType()
{
    return (DEVICE_BRAKE);
}

void CanBrake::loadConfiguration()
{
    CanBrakeConfiguration *config = (CanBrakeConfiguration *) getConfiguration();

    if (!config) { // as lowest sub-class make sure we have a config object
        config = new CanBrakeConfiguration();
        setConfiguration(config);
    }

    Throttle::loadConfiguration(); // call parent

#ifdef USE_HARD_CODED

    if (false) {
#else

    if (prefsHandler->checksumValid()) { //checksum is good, read in the values stored in EEPROM
#endif
        Logger::debug(CANBRAKEPEDAL, (char *) Constants::validChecksum);
        prefsHandler->read(EETH_MIN_ONE, &config->minimumLevel1);
        prefsHandler->read(EETH_MAX_ONE, &config->maximumLevel1);
        prefsHandler->read(EETH_CAR_TYPE, &config->carType);
    } else { //checksum invalid. Reinitialize values and store to EEPROM
        Logger::warn(CANBRAKEPEDAL, (char *) Constants::invalidChecksum);
        config->minimumLevel1 = 2;
        config->maximumLevel1 = 255;
        config->carType = Volvo_S80_Gas;
        saveConfiguration();
    }

    Logger::debug(CANBRAKEPEDAL, "T1 MIN: %l MAX: %l Type: %d", config->minimumLevel1, config->maximumLevel1, config->carType);
}

/*
 * Store the current configuration to EEPROM
 */
void CanBrake::saveConfiguration()
{
    CanBrakeConfiguration *config = (CanBrakeConfiguration *) getConfiguration();

    Throttle::saveConfiguration(); // call parent

    prefsHandler->write(EETH_MIN_ONE, config->minimumLevel1);
    prefsHandler->write(EETH_MAX_ONE, config->maximumLevel1);
    prefsHandler->write(EETH_CAR_TYPE, config->carType);
    prefsHandler->saveChecksum();
}
