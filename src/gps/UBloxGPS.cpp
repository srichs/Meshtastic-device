#include "configuration.h"
#include "UBloxGPS.h"
#include "RTC.h"
#include "error.h"
#include "sleep.h"
#include <assert.h>

// if gps_update_interval below this value, do not powercycle the GPS
#define UBLOX_POWEROFF_THRESHOLD 90

#define PDOP_INVALID 9999

extern RadioConfig radioConfig;

UBloxGPS::UBloxGPS() {}

bool UBloxGPS::tryConnect()
{
    bool c = false;

    if (_serial_gps)
        c = ublox.begin(*_serial_gps);

    if (!c && i2cAddress) {
        extern bool neo6M; // Super skanky - if we are talking to the device i2c we assume it is a neo7 on a RAK815, which
                           // supports the newer API
        neo6M = true;

        c = ublox.begin(Wire, i2cAddress);
    }

    if (c)
        setConnected();

    return c;
}

bool UBloxGPS::setupGPS()
{
    GPS::setupGPS();

    // uncomment to see debug info
    // ublox.enableDebugging(Serial);

    // try a second time, the ublox lib serial parsing is buggy?
    // see https://github.com/meshtastic/Meshtastic-device/issues/376
    for (int i = 0; (i < 3) && !tryConnect(); i++)
        delay(500);

    if (isConnected()) {
        DEBUG_MSG("Connected to UBLOX GPS successfully\n");

        if (!setUBXMode())
            RECORD_CRITICALERROR(CriticalErrorCode_UBloxInitFailed); // Don't halt the boot if saving the config fails, but do report the bug

        return true;
    } else {
        return false;
    }
}

bool UBloxGPS::setUBXMode()
{
    if (_serial_gps) {
        if (!ublox.setUART1Output(COM_TYPE_UBX, 1000)) // Use native API
            return false;
    }
    if (i2cAddress) {
        if (!ublox.setI2COutput(COM_TYPE_UBX, 1000))
            return false;
    }

    if (!ublox.setNavigationFrequency(1, 1000)) // Produce 4x/sec to keep the amount of time we stall in getPVT low
        return false;

    // ok = ublox.setAutoPVT(false); // Not implemented on NEO-6M
    // assert(ok);
    // ok = ublox.setDynamicModel(DYN_MODEL_BIKE); // probably PEDESTRIAN but just in case assume bike speeds
    // assert(ok);

    // per https://github.com/meshtastic/Meshtastic-device/issues/376 powerSaveMode might not work with the marginal
    // TTGO antennas
    // if (!ublox.powerSaveMode(true, 2000)) // use power save mode, the default timeout (1100ms seems a bit too tight)
    //     return false;

    if (!ublox.saveConfiguration(3000))
        return false;

    return true;
}

/**
 * Reset our GPS back to factory settings
 *
 * @return true for success
 */
bool UBloxGPS::factoryReset()
{
    bool ok = false;

    // It is useful to force back into factory defaults (9600baud, NMEA to test the behavior of boards that don't have
    // GPS_TX connected)
    ublox.factoryReset();
    delay(5000);
    tryConnect(); // sets isConnected

    // try a second time, the ublox lib serial parsing is buggy?
    for (int i = 0; (i < 3) && !tryConnect(); i++)
        delay(500);

    DEBUG_MSG("GPS Factory reset success=%d\n", isConnected());
    if (isConnected())
        ok = setUBXMode();

    return ok;
}

/** Idle processing while GPS is looking for lock */
void UBloxGPS::whileActive()
{
    ublox.flushPVT();  // reset ALL freshness flags first

    ublox.getT(maxWait()); // ask for new time data - hopefully ready when we come back

    // Ask for a new position fix - hopefully it will have results ready by next time
    // the order here is important, because we only check for has latitude when reading

    //ublox.getSIV(maxWait());  // redundant with getPDOP below
    ublox.getPDOP(maxWait());  // will trigger getSOL on NEO6, getP on others
    ublox.getP(maxWait());     // will trigger getPosLLH on NEO6, getP on others

    // the fixType flag will be checked and updated in lookForLocation()
}

/**
 * Perform any processing that should be done only while the GPS is awake and looking for a fix.
 * Override this method to check for new locations
 *
 * @return true if we've acquired a new location
 */
bool UBloxGPS::lookForTime()
{
    if (ublox.moduleQueried.gpsSecond) {
        /* Convert to unix time
        The Unix epoch (or Unix time or POSIX time or Unix timestamp) is the number of seconds that have elapsed since January
        1, 1970 (midnight UTC/GMT), not counting leap seconds (in ISO 8601: 1970-01-01T00:00:00Z).
        */
        struct tm t;
        t.tm_sec = ublox.getSecond(0);
        t.tm_min = ublox.getMinute(0);
        t.tm_hour = ublox.getHour(0);
        t.tm_mday = ublox.getDay(0);
        t.tm_mon = ublox.getMonth(0) - 1;
        t.tm_year = ublox.getYear(0) - 1900;
        t.tm_isdst = false;
        perhapsSetRTC(RTCQualityGPS, t);
        return true;
    }

    return false;
}

/**
 * Perform any processing that should be done only while the GPS is awake and looking for a fix.
 * Override this method to check for new locations
 *
 * @return true if we've acquired a new location
 */
bool UBloxGPS::lookForLocation()
{
    bool foundLocation = false;

    // catch fixType changes here, instead of whileActive()
    if (ublox.moduleQueried.fixType) {
        fixType = ublox.getFixType();
    }

    // check if GPS has an acceptable lock
    if (! hasLock()) {
        return false;
    }

    // check if a complete GPS solution set is available for reading
    // (some of these, like lat/lon are redundant and can be removed)
    if ( ! (ublox.moduleQueried.latitude && 
            ublox.moduleQueried.longitude &&
            ublox.moduleQueried.altitude &&
            ublox.moduleQueried.pDOP &&
            ublox.moduleQueried.gpsiTOW)) 
    {
        // Not ready? No problem! We'll try again later.
        return false;
    }

    // read lat/lon/alt/dop data into temporary variables to avoid
    // overwriting global variables with potentially invalid data
    int32_t tmp_dop = ublox.getPDOP(0); // PDOP (an accuracy metric) is reported in 10^2 units so we have to scale down when we use it
    int32_t tmp_lat = ublox.getLatitude(0);
    int32_t tmp_lon = ublox.getLongitude(0);
    int32_t tmp_alt_msl = ublox.getAltitudeMSL(0);
    int32_t tmp_alt_hae = ublox.getAltitude(0);
    // Note: heading is only currently implmented in the ublox for the 8m chipset - therefore
    // don't read it here - it will generate an ignored getPVT command on the 6ms
    // heading = ublox.getHeading(0);

    // read positional timestamp
    struct tm t;
    t.tm_sec = ublox.getSecond(0);
    t.tm_min = ublox.getMinute(0);
    t.tm_hour = ublox.getHour(0);
    t.tm_mday = ublox.getDay(0);
    t.tm_mon = ublox.getMonth(0) - 1;
    t.tm_year = ublox.getYear(0) - 1900;
    t.tm_isdst = false;

    time_t tmp_ts = mktime(&t);

    // SIV number is nice-to-have if it's available
    if (ublox.moduleQueried.SIV) {
        uint16_t gSIV = ublox.getSIV(0);
        setNumSatellites(gSIV);
    }

    // bogus lat lon is reported as 0 or 0 (can be bogus just for one)
    // Also: apparently when the GPS is initially reporting lock it can output a bogus latitude > 90 deg!
    // FIXME - NULL ISLAND is a real location on Earth!
    foundLocation = (tmp_lat != 0) && (tmp_lon != 0) && 
                    (tmp_lat <= 900000000) && (tmp_lat >= -900000000) &&
                    (tmp_dop < PDOP_INVALID);

    // only if entire dataset is valid, update globals from temp vars
    if (foundLocation) {
        longitude = tmp_lon;
        latitude = tmp_lat;
#ifdef GPS_ALTITUDE_HAE
        altitude = tmp_alt_hae / 1000;
#else
        altitude = tmp_alt_msl / 1000;
#endif
        geoidal_height = (tmp_alt_hae - tmp_alt_msl) / 1000;
        pos_timestamp = tmp_ts;
        dop = tmp_dop;
    } else {
        DEBUG_MSG("Invalid location discarded\n");
    }

    return foundLocation;
}

bool UBloxGPS::hasLock()
{
    return (fixType >= 3 && fixType <= 4);
}

bool UBloxGPS::whileIdle()
{
    // if using i2c or serial look too see if any chars are ready
    return ublox.checkUblox(); // See if new data is available. Process bytes as they come in.
}

/// If possible force the GPS into sleep/low power mode
/// Note: ublox doesn't need a wake method, because as soon as we send chars to the GPS it will wake up
void UBloxGPS::sleep()
{
    if (radioConfig.preferences.gps_update_interval > UBLOX_POWEROFF_THRESHOLD) {
        // Tell GPS to power down until we send it characters on serial port (we leave vcc connected)
        ublox.powerOff();
        // setGPSPower(false);
    }
}

void UBloxGPS::wake()
{
    if (radioConfig.preferences.gps_update_interval > UBLOX_POWEROFF_THRESHOLD) {
        fixType = 0; // assume we have no fix yet
    }

    // this is idempotent
    setGPSPower(true);

    // Note: no delay needed because now we leave gps power on always and instead use ublox.powerOff()
    // Give time for the GPS to boot
    // delay(200);
}