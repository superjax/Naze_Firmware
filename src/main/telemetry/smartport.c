/*
 * SmartPort Telemetry implementation by frank26080115
 * see https://github.com/frank26080115/cleanflight/wiki/Using-Smart-Port
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>

#include "platform.h"

#ifdef TELEMETRY

#include "common/axis.h"
#include "common/color.h"
#include "common/maths.h"

#include "drivers/system.h"
#include "drivers/accgyro.h"
#include "drivers/serial.h"
#include "drivers/bus_i2c.h"
#include "drivers/gpio.h"
#include "drivers/timer.h"
#include "drivers/pwm_rx.h"
#include "drivers/adc.h"
#include "drivers/light_led.h"

#include "flight/flight.h"
#include "flight/mixer.h"
#include "flight/failsafe.h"
#include "flight/navigation.h"
#include "rx/rx.h"
#include "rx/msp.h"
#include "io/escservo.h"
#include "io/rc_controls.h"
#include "io/gps.h"
#include "io/gimbal.h"
#include "io/serial.h"
#include "io/ledstrip.h"
#include "sensors/boardalignment.h"
#include "sensors/sensors.h"
#include "sensors/battery.h"
#include "sensors/acceleration.h"
#include "sensors/barometer.h"
#include "sensors/compass.h"
#include "sensors/gyro.h"

#include "telemetry/telemetry.h"
#include "telemetry/smartport.h"

#include "config/runtime_config.h"
#include "config/config.h"
#include "config/config_profile.h"
#include "config/config_master.h"

enum
{
    SPSTATE_UNINITIALIZED,
    SPSTATE_INITIALIZED,
    SPSTATE_WORKING,
    SPSTATE_TIMEDOUT,
    SPSTATE_DEINITIALIZED,
};

enum
{
    FSSP_START_STOP = 0x7E,
    FSSP_DATA_FRAME = 0x10,

    // ID of sensor. Must be something that is polled by FrSky RX
    FSSP_SENSOR_ID1 = 0x1B,
    FSSP_SENSOR_ID2 = 0x0D,
    FSSP_SENSOR_ID3 = 0x34,
    FSSP_SENSOR_ID4 = 0x67,
    // reverse engineering tells me that there are plenty more IDs
};

// these data identifiers are obtained from http://diydrones.com/forum/topics/amp-to-frsky-x8r-sport-converter
enum
{
    FSSP_DATAID_SPEED      = 0x0830 ,
    FSSP_DATAID_VFAS       = 0x0210 ,
    FSSP_DATAID_CURRENT    = 0x0200 ,
    FSSP_DATAID_RPM        = 0x050F ,
    FSSP_DATAID_ALTITUDE   = 0x0100 ,
    FSSP_DATAID_FUEL       = 0x0600 ,
    FSSP_DATAID_ADC1       = 0xF102 ,
    FSSP_DATAID_ADC2       = 0xF103 ,
    FSSP_DATAID_LATLONG    = 0x0800 ,
    FSSP_DATAID_CAP_USED   = 0x0600 ,
    FSSP_DATAID_VARIO      = 0x0110 ,
    FSSP_DATAID_CELLS      = 0x0300 ,
    FSSP_DATAID_CELLS_LAST = 0x030F ,
    FSSP_DATAID_HEADING    = 0x0840 ,
    FSSP_DATAID_ACCX       = 0x0700 ,
    FSSP_DATAID_ACCY       = 0x0710 ,
    FSSP_DATAID_ACCZ       = 0x0720 ,
    FSSP_DATAID_T1         = 0x0400 ,
    FSSP_DATAID_T2         = 0x0410 ,
    FSSP_DATAID_GPS_ALT    = 0x0820 ,
};

const uint16_t frSkyDataIdTable[] = {
    FSSP_DATAID_SPEED     ,
    FSSP_DATAID_VFAS      ,
    FSSP_DATAID_CURRENT   ,
    //FSSP_DATAID_RPM       ,
    FSSP_DATAID_ALTITUDE  ,
    FSSP_DATAID_FUEL      ,
    //FSSP_DATAID_ADC1      ,
    //FSSP_DATAID_ADC2      ,
    FSSP_DATAID_LATLONG   ,
    FSSP_DATAID_LATLONG   , // twice
    //FSSP_DATAID_CAP_USED  ,
    FSSP_DATAID_VARIO     ,
    //FSSP_DATAID_CELLS     ,
    //FSSP_DATAID_CELLS_LAST,
    FSSP_DATAID_HEADING   ,
    FSSP_DATAID_ACCX      ,
    FSSP_DATAID_ACCY      ,
    FSSP_DATAID_ACCZ      ,
    FSSP_DATAID_T1        ,
    FSSP_DATAID_T2        ,
    FSSP_DATAID_GPS_ALT   ,
    0
};

#define __USE_C99_MATH // for roundf()
#define SMARTPORT_BAUD 57600
#define SMARTPORT_UART_MODE MODE_BIDIR
#define SMARTPORT_SERVICE_DELAY_MS 5 // telemetry requests comes in at roughly 12 ms intervals, keep this under that
#define SMARTPORT_NOT_CONNECTED_TIMEOUT_MS 7000

static serialPort_t *mySerPort;
static telemetryConfig_t *telemetryConfig;
static portMode_t previousPortMode;
static uint32_t previousBaudRate;
extern void serialInit(serialConfig_t *); // from main.c // FIXME remove this dependency

char smartPortState = SPSTATE_UNINITIALIZED;
static uint8_t smartPortHasRequest = 0;
static uint8_t smartPortIdCnt = 0;
static uint32_t smartPortLastRequestTime = 0;
static uint32_t smartPortLastServiceTime = 0;

static void smartPortDataReceive(uint16_t c)
{
    uint32_t now = millis();

    // look for a valid request sequence
    static uint8_t lastChar;
    if (lastChar == FSSP_START_STOP) {
        smartPortState = SPSTATE_WORKING;
        smartPortLastRequestTime = now;
        if ((c == FSSP_SENSOR_ID1) ||
            (c == FSSP_SENSOR_ID2) ||
            (c == FSSP_SENSOR_ID3) ||
            (c == FSSP_SENSOR_ID4)) {
            smartPortHasRequest = 1;
            // we only responde to these IDs
            // the X4R-SB does send other IDs, we ignore them, but take note of the time
        }
    }
    lastChar = c;
}

static void smartPortSendByte(uint8_t c, uint16_t *crcp)
{
    // smart port escape sequence
    if (c == 0x7D || c == 0x7E) {
        serialWrite(mySerPort, 0x7D);
        c ^= 0x20;
    }

    serialWrite(mySerPort, c);

    if (crcp == NULL)
        return;

    uint16_t crc = *crcp;
    crc += c;
    crc += crc >> 8;
    crc &= 0x00FF;
    crc += crc >> 8;
    crc &= 0x00FF;
    *crcp = crc;
}

static void smartPortSendPackage(uint16_t id, uint32_t val)
{
    uint16_t crc = 0;
    smartPortSendByte(FSSP_DATA_FRAME, &crc);
    uint8_t *u8p = (uint8_t*)&id;
    smartPortSendByte(u8p[0], &crc);
    smartPortSendByte(u8p[1], &crc);
    u8p = (uint8_t*)&val;
    smartPortSendByte(u8p[0], &crc);
    smartPortSendByte(u8p[1], &crc);
    smartPortSendByte(u8p[2], &crc);
    smartPortSendByte(u8p[3], &crc);
    smartPortSendByte(0xFF - (uint8_t)crc, NULL);

    smartPortLastServiceTime = millis();
}

void initSmartPortTelemetry(telemetryConfig_t *initialTelemetryConfig)
{
    telemetryConfig = initialTelemetryConfig;
}

void freeSmartPortTelemetryPort(void)
{
    if (smartPortState == SPSTATE_UNINITIALIZED)
        return;
    if (smartPortState == SPSTATE_DEINITIALIZED)
        return;

    if (isTelemetryPortShared()) {
        endSerialPortFunction(mySerPort, FUNCTION_SMARTPORT_TELEMETRY);
        smartPortState = SPSTATE_DEINITIALIZED;
        serialInit(&masterConfig.serialConfig);
    }
    else {
        serialSetMode(mySerPort, previousPortMode);
        serialSetBaudRate(mySerPort, previousBaudRate);
        endSerialPortFunction(mySerPort, FUNCTION_SMARTPORT_TELEMETRY);
        smartPortState = SPSTATE_DEINITIALIZED;
    }
    mySerPort = NULL;
}

void configureSmartPortTelemetryPort(void)
{
    if (smartPortState != SPSTATE_UNINITIALIZED) {
        // do not allow reinitialization
        return;
    }

    mySerPort = findOpenSerialPort(FUNCTION_SMARTPORT_TELEMETRY);
    if (mySerPort) {
        previousPortMode = mySerPort->mode;
        previousBaudRate = mySerPort->baudRate;

        //waitForSerialPortToFinishTransmitting(mySerPort); // FIXME locks up the system

        serialSetBaudRate(mySerPort, SMARTPORT_BAUD);
        serialSetMode(mySerPort, SMARTPORT_UART_MODE);
        beginSerialPortFunction(mySerPort, FUNCTION_SMARTPORT_TELEMETRY);
    } else {
        mySerPort = openSerialPort(FUNCTION_SMARTPORT_TELEMETRY, NULL, SMARTPORT_BAUD, SMARTPORT_UART_MODE, telemetryConfig->telemetry_inversion);

        if (mySerPort) {
            smartPortState = SPSTATE_INITIALIZED;
            previousPortMode = mySerPort->mode;
            previousBaudRate = mySerPort->baudRate;
        }
        else {
            // failed, resume MSP and CLI
            if (isTelemetryPortShared()) {
                smartPortState = SPSTATE_DEINITIALIZED;
                serialInit(&masterConfig.serialConfig);
            }
        }
    }
}

bool canSendSmartPortTelemetry(void)
{
    return smartPortState == SPSTATE_INITIALIZED || smartPortState == SPSTATE_WORKING;
}

bool canSmartPortAllowOtherSerial(void)
{
    return smartPortState == SPSTATE_DEINITIALIZED;
}

bool isSmartPortTimedOut(void)
{
    return smartPortState >= SPSTATE_TIMEDOUT;
}

uint32_t getSmartPortTelemetryProviderBaudRate(void)
{
    return SMARTPORT_BAUD;
}

void handleSmartPortTelemetry(void)
{
    if (!canSendSmartPortTelemetry())
        return;

    while (serialTotalBytesWaiting(mySerPort) > 0) {
        uint8_t c = serialRead(mySerPort);
        smartPortDataReceive(c);
    }

    uint32_t now = millis();

    // if timed out, reconfigure the UART back to normal so the GUI or CLI works
    if ((now - smartPortLastRequestTime) > SMARTPORT_NOT_CONNECTED_TIMEOUT_MS) {
        smartPortState = SPSTATE_TIMEDOUT;
        return;
    }

    // limit the rate at which we send responses, we don't want to affect flight characteristics (but USART1 is using DMA so I doubt it matters)
    if ((now - smartPortLastServiceTime) < SMARTPORT_SERVICE_DELAY_MS)
        return;

    if (smartPortHasRequest) {
        // we can send back any data we want, our table keeps track of the order and frequency of each data type we send
        uint16_t id = frSkyDataIdTable[smartPortIdCnt];
        if (id == 0) { // end of table reached, loop back
            smartPortIdCnt = 0;
            id = frSkyDataIdTable[smartPortIdCnt];
        }
        smartPortIdCnt++;

        float tmpf;
        int32_t tmpi;
        uint32_t tmpui;
        static uint8_t t1Cnt = 0;

        switch(id) {
            case FSSP_DATAID_SPEED      :
                if (sensors(SENSOR_GPS) && STATE(GPS_FIX)) {
                    tmpf = GPS_speed;
                    tmpf *= 0.36;
                    smartPortSendPackage(id, (uint32_t)lroundf(tmpf)); // given in 0.1 m/s, provide in KM/H
                    smartPortHasRequest = 0;
                }
                break;
            case FSSP_DATAID_VFAS       :
                smartPortSendPackage(id, vbat * 83); // supposedly given in 0.1V, unknown requested unit
                // multiplying by 83 seems to make Taranis read correctly
                smartPortHasRequest = 0;
                break;
            case FSSP_DATAID_CURRENT    :
                smartPortSendPackage(id, amperage); // given in 10mA steps, unknown requested unit
                smartPortHasRequest = 0;
                break;
            //case FSSP_DATAID_RPM        :
            case FSSP_DATAID_ALTITUDE   :
                smartPortSendPackage(id, BaroAlt); // unknown given unit, requested 100 = 1 meter
                smartPortHasRequest = 0;
                break;
            case FSSP_DATAID_FUEL       :
                smartPortSendPackage(id, mAhDrawn); // given in mAh, unknown requested unit
                smartPortHasRequest = 0;
                break;
            //case FSSP_DATAID_ADC1       :
            //case FSSP_DATAID_ADC2       :
            case FSSP_DATAID_LATLONG    :
                if (sensors(SENSOR_GPS) && STATE(GPS_FIX)) {
                    tmpui = 0;
                    // the same ID is sent twice, one for longitude, one for latitude
                    // the MSB of the sent uint32_t helps FrSky keep track
                    // the even/odd bit of our counter helps us keep track                    
                    if (smartPortIdCnt & 1) {
                        tmpui = tmpi = GPS_coord[LON];
                        if (tmpi < 0) {
                            tmpui = -tmpi;
                            tmpui |= 0x40000000;
                        }
                        tmpui |= 0x80000000;
                    }
                    else {
                        tmpui = tmpi = GPS_coord[LAT];
                        if (tmpi < 0) {
                            tmpui = -tmpi;
                            tmpui |= 0x40000000;
                        }
                    }
                    smartPortSendPackage(id, tmpui);
                    smartPortHasRequest = 0;
                }
                break;
            //case FSSP_DATAID_CAP_USED   :
            case FSSP_DATAID_VARIO      :
                smartPortSendPackage(id, vario); // unknown given unit but requested in 100 = 1m/s
                smartPortHasRequest = 0;
                break;
            case FSSP_DATAID_HEADING    :
                smartPortSendPackage(id, heading / 10); // given in 0.1 deg, requested in 10000 = 100 deg
                smartPortHasRequest = 0;
                break;
            case FSSP_DATAID_ACCX       :
                smartPortSendPackage(id, accSmooth[X] / 44);
                // unknown input and unknown output unit
                // we can only show 00.00 format, another digit won't display right on Taranis
                // dividing by roughly 44 will give acceleration in G units
                smartPortHasRequest = 0;
                break;
            case FSSP_DATAID_ACCY       :
                smartPortSendPackage(id, accSmooth[Y] / 44);
                smartPortHasRequest = 0;
                break;
            case FSSP_DATAID_ACCZ       :
                smartPortSendPackage(id, accSmooth[Z] / 44);
                smartPortHasRequest = 0;
                break;
            case FSSP_DATAID_T1         :
                // we send all the flags as decimal digits for easy reading

                // the t1Cnt simply allows the telemetry view to show at least some changes
                t1Cnt++;
                if (t1Cnt >= 4) {
                    t1Cnt = 1;
                }
                tmpi = t1Cnt * 10000; // start off with at least one digit so the most significant 0 won't be cut off
                // the Taranis seems to be able to fit 5 digits on the screen
                // the Taranis seems to consider this number a signed 16 bit integer

                if (ARMING_FLAG(OK_TO_ARM))
                    tmpi += 1;
                if (ARMING_FLAG(PREVENT_ARMING))
                    tmpi += 2;
                if (ARMING_FLAG(ARMED))
                    tmpi += 4;

                if (FLIGHT_MODE(ANGLE_MODE))
                    tmpi += 10;
                if (FLIGHT_MODE(HORIZON_MODE))
                    tmpi += 20;
                if (FLIGHT_MODE(AUTOTUNE_MODE))
                    tmpi += 40;
                if (FLIGHT_MODE(PASSTHRU_MODE))
                    tmpi += 40;

                if (FLIGHT_MODE(MAG_MODE))
                    tmpi += 100;
                if (FLIGHT_MODE(BARO_MODE))
                    tmpi += 200;
                if (FLIGHT_MODE(SONAR_MODE))
                    tmpi += 400;

                if (FLIGHT_MODE(GPS_HOLD_MODE))
                    tmpi += 1000;
                if (FLIGHT_MODE(GPS_HOME_MODE))
                    tmpi += 2000;
                if (FLIGHT_MODE(HEADFREE_MODE))
                    tmpi += 4000;

                smartPortSendPackage(id, (uint32_t)tmpi);
                smartPortHasRequest = 0;
                break;
            case FSSP_DATAID_T2         :
                if (sensors(SENSOR_GPS)) {
                    // provide GPS lock status
                    smartPortSendPackage(id, (STATE(GPS_FIX) ? 1000 : 0) + (STATE(GPS_FIX_HOME) ? 2000 : 0) + GPS_numSat);
                    smartPortHasRequest = 0;
                }
                else {
                    smartPortSendPackage(id, 0);
                    smartPortHasRequest = 0;
                }
                break;
            case FSSP_DATAID_GPS_ALT    :
                if (sensors(SENSOR_GPS) && STATE(GPS_FIX)) {
                    smartPortSendPackage(id, GPS_altitude * 1000); // given in 0.1m , requested in 100 = 1m
                    smartPortHasRequest = 0;
                }
                break;
            default:
                break;
                // if nothing is sent, smartPortHasRequest isn't cleared, we already incremented the counter, just wait for the next loop
        }
    }
}

#endif