/*
 * LittleBBS module – a simple message board that allows users to post short
 * messages to a shared channel.  Messages are broadcast on the primary channel
 * and displayed on all nodes with the module enabled.  To prevent spam, each
 * sender is rate‑limited to one message per minute.
 *
 * To enable this module, set `#undef MESHTASTIC_EXCLUDE_LITTLEBBS` in your variant.h file.
 */
#include "configuration.h"
#if !MESHTASTIC_EXCLUDE_LITTLEBBS
#include "Channels.h"
#include "LittleBBSModule.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "mesh/MeshTypes.h"
#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <cctype>
#include <cstring>

static float jsonArrayNth(const char *tag, int n);
static const char *wmoToEmoji(int code);

LittleBBSModule *littleBBSModule;
// Constructor – registers a single text port and marks the module promiscuous
// so that broadcast messages on the primary channel are visible.
LittleBBSModule::LittleBBSModule()
    : SinglePortModule("littlebbs", meshtastic_PortNum_TEXT_MESSAGE_APP), concurrency::OSThread("littlebbs_thread", 60000)
{
    isPromiscuous = true;
}

void LittleBBSModule::setup()
{
    // In future we may add a protobuf configuration; for now the module is
    // always enabled when compiled in.
    LOG_INFO("[LittleBBS] LittleBBS module enabled");
}

int32_t LittleBBSModule::runOnce()
{
    return 60000;
}

// Determine whether we want to process this packet.  We only care about
// plain text messages addressed to our port.
bool LittleBBSModule::wantPacket(const meshtastic_MeshPacket *p)
{
    return (p && p->decoded.portnum == ourPortNum);
}

ProcessMessage LittleBBSModule::handleReceived(const meshtastic_MeshPacket &mp)
{

    const uint32_t ourNode = nodeDB->getNodeNum();
    if (mp.from == ourNode) {
        return ProcessMessage::CONTINUE;
    }
    const bool isDM = (mp.to == ourNode);

    // Ignore empty payloads
    if (mp.decoded.payload.size == 0) {
        return ProcessMessage::CONTINUE;
    }

    // Accept only direct messages
    if (!isDM) {
        return ProcessMessage::CONTINUE;
    }

    char buf[260];
    memset(buf, 0, sizeof(buf));
    size_t n = mp.decoded.payload.size;
    if (n > sizeof(buf) - 1)
        n = sizeof(buf) - 1;
    memcpy(buf, mp.decoded.payload.bytes, n);

    const char *msg = buf;
    while (*msg == ' ' || *msg == '\t')
        msg++;

    // Only reply to explicit commands to avoid bot-to-bot feedback loops.
    if ((strcmp(msg, "/menu") == 0) || strcmp(msg, "X") == 0 || strcmp(msg, "x") == 0) {
        LOG_INFO("[LittleBBS] Sending main menu");
        sendMainMenu(mp);
        return ProcessMessage::CONTINUE;
    }

    if (strcmp(msg, "/help") == 0 || strcmp(msg, "/bbs") == 0) {
        LOG_INFO("[LittleBBS] Sending help message");
        sendDm(mp, "Hello I'm a LittleBBS. Send /menu to see the main menu.");
        return ProcessMessage::CONTINUE;
    }

    // Respond to "ping" with a simple status message
    if (strcmp(msg, "P") == 0 || strcmp(msg, "p") == 0) {
        LOG_INFO("[LittleBBS] Received ping command, sending status");
        char reply[100];
        static const char *pingEmoji = "\xF0\x9F\x8F\x93";
        snprintf(reply, sizeof(reply), "%s Pong! Node %x is here and alive.", pingEmoji, ourNode);
        sendDm(mp, reply);
        return ProcessMessage::CONTINUE;
    }

    // Respond to "QTH" with our current location if we have it, or an error message if we don't
    if (strcmp(msg, "/QTH?") == 0 || strcmp(msg, "/qth?") == 0 || strcmp(msg, "Q") == 0 || strcmp(msg, "q") == 0) {
        LOG_INFO("[LittleBBS] Received QTH command, sending location");
        float lat = 0.0f, lon = 0.0f;
        //Get our coordinates from the NodeDB - this will be based on the last position we received from the radio 
        // (which might be our own node if we have a GPS, or might be another node if we are using that node as a proxy for our location)
        meshtastic_PositionLite pos;
        if (nodeDB->copyNodePosition(nodeDB->getNodeNum(), pos)) {
            lat = pos.latitude_i / 1e7f;
            lon = pos.longitude_i / 1e7f;
        }
        char reply[100];
        if (lat != 0.0f && lon != 0.0f) {
            char city[64];
            char country[48];
            if (reverseGeocode(lat, lon, city, sizeof(city), country, sizeof(country))) {
                snprintf(reply, sizeof(reply), "My location is %.4f, %.4f near %s, %s.", lat, lon, city, country[0] ? country : "Unknown Country");
            } else {
                snprintf(reply, sizeof(reply), "My location is %.4f, %.4f.", lat, lon);
            }
        } else {
            snprintf(reply, sizeof(reply), "Sorry, I don't know my location.");
        }
        sendDm(mp, reply);
        return ProcessMessage::CONTINUE;
    }

    // Respond to "test" with a message showing the signal quality
    if (strcmp(msg, "T") == 0 || strcmp(msg, "t") == 0) {
        LOG_INFO("[LittleBBS] Received test command, sending status");
        int hopsAway = getHopsAway(mp);
        int rssi = mp.rx_rssi;
        if (rssi > 0) {
            rssi -= 200;
        }
        float snr = mp.rx_snr;
        char reply[96];
        const uint32_t remoteNode = mp.from;
        snprintf(reply, sizeof(reply), "Hi %x! You are %d hops away from me | RSSI %d | SNR %.1f", remoteNode, hopsAway, rssi, snr);
        sendDm(mp, reply);
        return ProcessMessage::CONTINUE;
    }

    // Respond to "m" or "M" showing how to get meteo info
    if (strcmp(msg, "M") == 0 || strcmp(msg, "m") == 0) {
        LOG_INFO("[LittleBBS] Received meteo command, sending status");
        char reply[100];
        snprintf(reply, sizeof(reply), "Send /meteo <city> to get the weather for that city.");
        sendDm(mp, reply);
        return ProcessMessage::CONTINUE;
    }

    // Weather alert command. If no city is provided, try to infer sender location.
    if (strncmp(msg, "/alerts", 7) == 0 || strncmp(msg, "/meteoalerts", 12) == 0 || strcmp(msg, "A") == 0 || strcmp(msg, "a") == 0) {
        const char *city = nullptr;
        if (strncmp(msg, "/alerts", 7) == 0) {
            city = msg + 7;
        } else {
            city = msg + 12;
        }

        while (*city == ' ' || *city == '\t') {
            city++;
        }

        if (*city == '\0') {
            LOG_INFO("[LittleBBS] Received alerts command with no city, trying to determine sender location");
            float lat = 0.0f, lon = 0.0f;
            getRemoteNodeCoordinates(mp, lat, lon);
            if (lat != 0.0f && lon != 0.0f) {
                char inferredCity[64] = {0};
                char country[48] = {0};
                if (reverseGeocode(lat, lon, inferredCity, sizeof(inferredCity), country, sizeof(country)) && inferredCity[0] != '\0') {
                    LOG_DEBUG("[LittleBBS] Inferred city '%s' for coordinates lat=%.4f lon=%.4f\n", inferredCity, lat, lon);
                    String summary = getMeteoAlerts(inferredCity);
                    String message = "Richiesta di allerte meteo per " + String(inferredCity) + ": " + summary + ".\nDati da https://allertameteo.app/";
                    sendDm(mp, message.c_str());
                } else {
                    sendDm(mp,
                           "I can't determine your city for alerts. Send /alerts <city> or /meteoalerts <city>.");
                }
            } else {
                LOG_DEBUG("[LittleBBS] Unable to determine coordinates for sender node 0x%x\n", mp.from);
                sendDm(mp, "I can't determine your location for alerts. Send /alerts <city> or /meteoalerts <city>.");
            }
            return ProcessMessage::CONTINUE;
        }

        String summary = getMeteoAlerts(city);
        LOG_INFO("[LittleBBS] Sending alerts for city '%s'.", city);
        String message = "Richiesta di allerte meteo per " + String(city) + ": " + summary + ".\nDati da https://allertameteo.app/";
        sendDm(mp, message.c_str());
        return ProcessMessage::CONTINUE;
    }

    // Respond to command the starts with "/meteo".
    // If the command is "/meteo" with no city, try to determine the sender's location and send a weather report for that location
    if (strncmp(msg, "/meteo", 6) == 0) {
        const char *city = msg + 6;
        while (*city == ' ' || *city == '\t')
            city++;
        if (*city == '\0') {
            LOG_INFO("[LittleBBS] Received meteo command with no city, trying to determine sender location");

            float lat, lon;
            getRemoteNodeCoordinates(mp, lat, lon);

            if (lat != 0.0f && lon != 0.0f) {
                char reply[512];
                if (getWeatherForecast(reply, sizeof(reply), lat, lon)) {
                    LOG_DEBUG("[LittleBBS] Got weather forecast for coordinates lat=%.4f lon=%.4f\n", lat, lon);
                    sendDm(mp, reply);
                } else {
                    LOG_DEBUG("[LittleBBS] Unable to get weather forecast for coordinates lat=%.4f lon=%.4f\n", lat, lon);
                    sendDm(mp, "Unable to get weather forecast for your location. Send /meteo <city> to get the weather for a "
                               "specific city.");
                }
            } else {
                sendDm(mp,
                       "I can't determine the weather for your location. Send /meteo <city> to get the weather for that city.");
            }

            return ProcessMessage::CONTINUE;
        }
        LOG_INFO("[LittleBBS] Received meteo command for city '%s'.", city);
        char reply[100];
        float lat, lon;
        if (geocodeLookup(city, lat, lon)) {
            LOG_DEBUG("[LittleBBS] geocodeLookup found coordinates for city '%s': lat=%.4f lon=%.4f\n", city, lat, lon);
            if (getWeatherForecast(reply, sizeof(reply), lat, lon)) {
                LOG_DEBUG("[LittleBBS] Got weather forecast for city '%s' at coordinates lat=%.4f lon=%.4f\n", city, lat, lon);
                sendDm(mp, reply);
            } else {
                LOG_DEBUG("[LittleBBS] Unable to get weather forecast for city '%s' at coordinates lat=%.4f lon=%.4f\n", city, lat, lon);
                snprintf(reply, sizeof(reply), "Unable to get weather forecast for %s.", city);
                sendDm(mp, reply);
            }
        } else {
            LOG_DEBUG("[LittleBBS] geocodeLookup unable to find coordinates for city '%s'\n", city);
            snprintf(reply, sizeof(reply), "Unable to find that city: %s.", city);
            sendDm(mp, reply);
        }
        return ProcessMessage::CONTINUE;
    }

    LOG_DEBUG("[LittleBBS] Ignored non-command DM from 0x%x", mp.from);
    return ProcessMessage::CONTINUE;
}

// Get coordinates from contacting node
void LittleBBSModule::getRemoteNodeCoordinates(const meshtastic_MeshPacket &mp, float &lat, float &lon)
{
    lat = 0.0f;
    lon = 0.0f;
    meshtastic_PositionLite pos;
    if (nodeDB->copyNodePosition(mp.from, pos)) {
        lat = pos.latitude_i / 1e7f;
        lon = pos.longitude_i / 1e7f;
        LOG_DEBUG("[LittleBBS] Got coordinates for node 0x%x: lat=%.4f lon=%.4f\n", mp.from, lat, lon);
    } else {
        LOG_DEBUG("[LittleBBS] No coordinates available for node 0x%x\n", mp.from);
    }
}

bool LittleBBSModule::geocodeLookup(const char *location, float &lat, float &lon)
{
    lat = 0.0f;
    lon = 0.0f;
    bool parsedLatLon = false;

    if (WiFi.status() != WL_CONNECTED) {
        LOG_DEBUG("[LittleBBS] Geocode lookup skipped: WiFi not connected. Is WiFi enabled and configured on this node?\n");
        return false;
    }

    char gurl[256];
    snprintf(gurl, sizeof(gurl),
             "https://nominatim.openstreetmap.org/search"
             "?q=%s&format=json&limit=1",
             location);
    LOG_DEBUG("[LittleBBS] geocodeLookup calling URL %s\n", gurl);
    WiFiClientSecure gwc;
    gwc.setInsecure();
    HTTPClient ghttp;
    ghttp.setConnectTimeout(2500);
    ghttp.setTimeout(3500);
    if (!ghttp.begin(gwc, gurl)) {
        LOG_DEBUG("[LittleBBS] GeocodeLookup begin() failed");
        return false;
    }
    ghttp.addHeader("User-Agent", "LittleBBS/1.0");
    int gcode = ghttp.GET();
    if (gcode < 0) {
        LOG_WARN("[LittleBBS] GeocodeLookup - GET failed (%d): %s", gcode, ghttp.errorToString(gcode).c_str());
    }
    LOG_DEBUG("[LittleBBS] GeocodeLookup - nominatim code=%d\n", gcode);
    if (gcode == 200) {
        String gbody = ghttp.getString();
        LOG_DEBUG("[LittleBBS] GeocodeLookup - got response len=%u", static_cast<unsigned>(gbody.length()));
        const char *json = gbody.c_str();

        auto extractJsonFloatValue = [](const char *jsonIn, const char *fieldName, float &out) {
            if (!jsonIn || !fieldName) {
                return false;
            }

            const char *kp = strstr(jsonIn, fieldName);
            if (!kp) {
                return false;
            }

            const char *cp = strchr(kp + strlen(fieldName), ':');
            if (!cp) {
                return false;
            }
            cp++;
            while (*cp == ' ' || *cp == '\t' || *cp == '\n' || *cp == '\r') {
                cp++;
            }
            if (*cp == '"') {
                cp++;
            }

            char numbuf[24];
            size_t i = 0;
            while (*cp && *cp != '"' && *cp != ',' && *cp != '}' && i < sizeof(numbuf) - 1) {
                numbuf[i++] = *cp++;
            }
            numbuf[i] = '\0';
            if (i == 0) {
                return false;
            }

            out = atof(numbuf);
            return true;
        };

        const bool haveLat = extractJsonFloatValue(json, "\"lat\"", lat);
        const bool haveLon = extractJsonFloatValue(json, "\"lon\"", lon);
        parsedLatLon = haveLat && haveLon;
        if (!haveLat || !haveLon) {
            LOG_WARN("[LittleBBS] GeocodeLookup - missing lat/lon in response");
        }
        LOG_DEBUG("[LittleBBS] GeocodeLookup - got coordinates lat=%.4f lon=%.4f\n", lat, lon);
    } else {
        LOG_DEBUG("[LittleBBS] GeocodeLookup - HTTP error %d\n", gcode);
    }
    ghttp.end();
    gwc.stop();
    return parsedLatLon;
}
// Reverse geocode coordinates to get city and country using the OpenStreetMap Nominatim API
// The original function comes from https://github.com/GoatsAndMonkeys/TinyBBS
bool LittleBBSModule::reverseGeocode(float lat, float lon, char *city, size_t cityLen, char *country, size_t countryLen)
{
    city[0] = '\0';
    country[0] = '\0';

    if (WiFi.status() != WL_CONNECTED) {
        LOG_DEBUG("[LittleBBS] Geocode skipped: WiFi not connected. Is WiFi enabled and configured on this node?\n");
        return false;
    }

    char gurl[256];
    snprintf(gurl, sizeof(gurl),
             "https://nominatim.openstreetmap.org/reverse"
             "?lat=%.4f&lon=%.4f&format=json&zoom=9&addressdetails=1",
             lat, lon);
    LOG_DEBUG("[LittleBBS] geocode calling URL %s\n", gurl);
    WiFiClientSecure gwc;
    gwc.setInsecure();
    HTTPClient ghttp;
    ghttp.setConnectTimeout(2500);
    ghttp.setTimeout(3500);
    if (!ghttp.begin(gwc, gurl)) {
        LOG_DEBUG("[LittleBBS] Geocode begin() failed");
        return false;
    }
    ghttp.addHeader("User-Agent", "LittleBBS/1.0");
    int gcode = ghttp.GET();
    if (gcode < 0) {
        LOG_WARN("[LittleBBS] Geocode - GET failed (%d): %s", gcode, ghttp.errorToString(gcode).c_str());
    }
    LOG_DEBUG("[LittleBBS] Geocode - nominatim code=%d\n", gcode);
    if (gcode == 200) {
        String gbody = ghttp.getString();
        LOG_DEBUG("[LittleBBS] Geocode - got response len=%u", static_cast<unsigned>(gbody.length()));
        const char *json = gbody.c_str();
        auto extractJsonStringValue = [](const char *jsonIn, const char *fieldName, char *out, size_t outLen) {
            if (!jsonIn || !fieldName || !out || outLen == 0) {
                return;
            }

            const char *kp = strstr(jsonIn, fieldName);
            if (!kp) {
                return;
            }

            const char *cp = strchr(kp + strlen(fieldName), ':');
            if (!cp) {
                return;
            }
            cp++;
            while (*cp == ' ' || *cp == '\t' || *cp == '\n' || *cp == '\r') {
                cp++;
            }
            if (*cp != '"') {
                return;
            }
            cp++;

            const char *ce = strchr(cp, '"');
            if (!ce) {
                return;
            }
            size_t len = ce - cp;
            if (len >= outLen) {
                len = outLen - 1;
            }
            memcpy(out, cp, len);
            out[len] = '\0';
        };

        const char *cityKeys[] = {"\"city\"", "\"town\"", "\"village\"", "\"county\""};
        for (const char *key : cityKeys) {
            extractJsonStringValue(json, key, city, cityLen);
            if (city[0] != '\0') {
                break;
            }
        }

        extractJsonStringValue(json, "\"country\"", country, countryLen);
        if (country[0] != '\0') {
            LOG_DEBUG("[LittleBBS] Geocode - got city='%s' country='%s'\n", city, country);
        }
    } else {
        LOG_DEBUG("[LittleBBS] Geocode - HTTP error %d\n", gcode);
    }
    ghttp.end();
    gwc.stop();
    return city[0] != '\0' || country[0] != '\0';
}

// Get a weather report to be returned for a /meteo command.
// This original function comes from https://github.com/GoatsAndMonkeys/TinyBBS and has been modified to use the Open-Meteo API over https.
bool LittleBBSModule::getWeatherForecast(char *buf, size_t bufLen, float lat, float lon)
{
    if (WiFi.status() != WL_CONNECTED) {
        LOG_DEBUG("[LittleBBS] MeteoForecast skipped: WiFi not connected. Is WiFi enabled and configured on this node?\n");
        return false;
    }

    static char sbuf[2048];
    char url[256];
    String body;
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast"
             "?latitude=%.4f&longitude=%.4f"
             "&daily=temperature_2m_max,temperature_2m_min,precipitation_probability_max,weathercode"
             "&temperature_unit=celsius&timezone=auto&forecast_days=6",
             lat, lon);

    LOG_DEBUG("[LittleBBS] MeteoForecast calling URL %s\n", url);
    WiFiClientSecure gwc;
    gwc.setInsecure();
    HTTPClient mfhttp;
    mfhttp.setConnectTimeout(2500);
    mfhttp.setTimeout(3500);
    if (!mfhttp.begin(gwc, url)) {
        LOG_DEBUG("[LittleBBS] MeteoForecast begin() failed");
        return false;
    }
    mfhttp.addHeader("User-Agent", "LittleBBS/1.0");
    int mcode = -1;
    for (int attempt = 1; attempt <= 4; ++attempt) {
        mcode = mfhttp.GET();
        LOG_DEBUG("[LittleBBS] MeteoForecast - HTTP code=%d (attempt %d/4)\n", mcode, attempt);
        if (mcode != HTTPC_ERROR_CONNECTION_REFUSED || attempt == 4) {
            break;
        }
        LOG_DEBUG("[LittleBBS] MeteoForecast - connection refused, retrying...\n");
    }

    if (mcode == 200) {
        body = mfhttp.getString();
        LOG_DEBUG("[LittleBBS] MeteoForecast - got response len=%u", static_cast<unsigned>(body.length()));
        strncpy(sbuf, body.c_str(), sizeof(sbuf) - 1);
        sbuf[sizeof(sbuf) - 1] = '\0';
    } else {
        LOG_DEBUG("[LittleBBS] MeteoForecast - HTTP error %d\n", mcode);
    }

    if (mcode != 200) {
        LOG_DEBUG("[LittleBBS] MeteoForecast - HTTP request failed with code %d\n", mcode);
        mfhttp.end();
        gwc.stop();
        return false;
    }

    strncpy(sbuf, body.c_str(), sizeof(sbuf) - 1);
    sbuf[sizeof(sbuf) - 1] = '\0';

    LOG_DEBUG("[LittleBBS] MeteoForecast - body=%u bytes\n", (unsigned)body.length());

    // Parse Open-Meteo JSON — arrays have 3 values (one per day)
    const char *tmax = strstr(sbuf, "\"temperature_2m_max\":[");
    const char *tmin = strstr(sbuf, "\"temperature_2m_min\":[");
    const char *prec = strstr(sbuf, "\"precipitation_probability_max\":[");
    const char *wcod = strstr(sbuf, "\"weathercode\":[");

    if (!tmax || !tmin) {
        LOG_DEBUG("[LittleBBS] MeteoForecast - missing expected data in response\n");
        mfhttp.end();
        gwc.stop();
        return false;
    }

    // Day labels: parse "time":["YYYY-MM-DD",...] from response
    // Fall back to Today/+1/+2 from RTC if parsing fails
    static const char *dow[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    const char *labels[6] = {"Today", "Day2", "Day3", "Day4", "Day5", "Day6"};
    {
        auto dateToDow = [](const char *d) -> int {
            int y = atoi(d), m = atoi(d + 5), day = atoi(d + 8);
            if (m < 3) {
                m += 12;
                y--;
            }
            int k = y % 100, j = y / 100;
            int h = (day + (13 * (m + 1)) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;
            return (h + 6) % 7;
        };
        const char *tp = strstr(sbuf, "\"time\":[\"");
        if (tp) {
            tp += 9;
            for (int i = 0; i < 6; i++) {
                if (tp[0] && tp[1] && tp[2] && tp[3] && tp[4] && tp[5] && tp[6] && tp[7] && tp[8] && tp[9]) {
                    int d = dateToDow(tp);
                    labels[i] = (i == 0) ? "Today" : dow[d];
                }
                while (*tp && *tp != '"')
                    tp++;
                if (*tp == '"')
                    tp++;
                if (*tp == ',')
                    tp++;
                if (*tp == '"')
                    tp++;
            }
        }
    }

    char city[48] = {0};
    char country[48] = {0};

    if (reverseGeocode(lat, lon, city, sizeof(city), country, sizeof(country))) {
        LOG_DEBUG("[LittleBBS] MeteoForecast - reverse geocoded coordinates lat=%.4f lon=%.4f to city '%s', country '%s'\n", lat, lon, city, country);
    } else {
        LOG_DEBUG("[LittleBBS] MeteoForecast - unable to reverse geocode coordinates lat=%.4f lon=%.4f\n", lat, lon);
        return false;
    }

    size_t pos = 0;
    if (city[0])
        pos += snprintf(buf + pos, bufLen - pos, "TinyBBS 5Day 4Cast (%s, %s):\n", city, country[0] ? country : "Unknown Country");
    else
        pos += snprintf(buf + pos, bufLen - pos, "TinyBBS 5Day 4Cast:\n");
    for (int i = 0; i < 6 && pos < bufLen - 1; i++) {
        float hi = jsonArrayNth(tmax, i);
        float lo = jsonArrayNth(tmin, i);
        int pp = prec ? (int)(jsonArrayNth(prec, i) + 0.5f) : 0;
        int wc = wcod ? (int)(jsonArrayNth(wcod, i) + 0.5f) : 0;
        const char *em = wmoToEmoji(wc);
        if (i == 0) {
            pos += snprintf(buf + pos, bufLen - pos, "Today L/H %d\xC2\xB0/%d\xC2\xB0 Precip %d%% Cond %s\n", (int)(lo + 0.5f),
                            (int)(hi + 0.5f), pp, em);
        } else {
            pos += snprintf(buf + pos, bufLen - pos, "%s %d\xC2\xB0/%d\xC2\xB0 %d%%%s\n", labels[i], (int)(lo + 0.5f),
                            (int)(hi + 0.5f), pp, em);
        }
    }
    if (pos > 0 && buf[pos - 1] == '\n')
        buf[--pos] = '\0';

    mfhttp.end();
    gwc.stop();
    return true;
}

// Create main BBS menu and send it back to the sender via DM
void LittleBBSModule::sendMainMenu(const meshtastic_MeshPacket &mp)
{
    char menu[200];
    snprintf(menu, sizeof(menu),
             "Welcome to the LittleBBS!\n\n"
             "[A]lerte meteo\n"
             "[P]ing\n"
             "[T]est\n"
             "[Q]TH?\n"
             "[M]eteo\n\n"
             "Reply with the letter in brackets to get a response.");
    sendDm(mp, menu);
}

// Send a direct message back to the originating node.
void LittleBBSModule::sendDm(const meshtastic_MeshPacket &rx, const char *text)
{
    if (!text)
        return;
    meshtastic_MeshPacket *p = allocDataPacket();
    if (!p) {
        LOG_WARN("[LittleBBS] Unable to allocate reply packet");
        return;
    }
    p->to = rx.from;
    p->channel = rx.channel;
    p->want_ack = false;
    p->decoded.want_response = false;
    size_t len = strlen(text);
    if (len > sizeof(p->decoded.payload.bytes)) {
        len = sizeof(p->decoded.payload.bytes);
    }
    p->decoded.payload.size = len;
    memcpy(p->decoded.payload.bytes, text, len);
    service->sendToMesh(p);
}

// Parse json from https://allertameteo.app/api/alert/
// 
String LittleBBSModule::getMeteoAlerts(const char *city)
{
    const char *unknownSummary = "today - unknown | tomorrow - unknown";

    if (!city) {
        return String(unknownSummary);
    }

    while (*city == ' ' || *city == '\t') {
        city++;
    }
    if (*city == '\0') {
        return String(unknownSummary);
    }

    if (WiFi.status() != WL_CONNECTED) {
        LOG_DEBUG("[LittleBBS] MeteoAlerts skipped: WiFi not connected. Is WiFi enabled and configured on this node?\n");
        return String(unknownSummary);
    }

    const char *cityEnd = city + strlen(city);
    while (cityEnd > city && (cityEnd[-1] == ' ' || cityEnd[-1] == '\t')) {
        cityEnd--;
    }
    if (cityEnd <= city) {
        return String(unknownSummary);
    }

    char encodedCity[192];
    size_t ep = 0;
    for (const char *p = city; p < cityEnd && ep < sizeof(encodedCity) - 1; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        const bool isSafe = isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
        if (isSafe) {
            encodedCity[ep++] = static_cast<char>(c);
            continue;
        }
        if (ep + 3 >= sizeof(encodedCity)) {
            LOG_WARN("[LittleBBS] MeteoAlerts city too long after URL encoding");
            return String(unknownSummary);
        }
        static const char hex[] = "0123456789ABCDEF";
        encodedCity[ep++] = '%';
        encodedCity[ep++] = hex[(c >> 4) & 0x0F];
        encodedCity[ep++] = hex[c & 0x0F];
    }
    encodedCity[ep] = '\0';
    if (encodedCity[0] == '\0') {
        return String(unknownSummary);
    }

    char url[320];
    snprintf(url, sizeof(url), "https://allertameteo.app/api/alert/%s", encodedCity);
    LOG_DEBUG("[LittleBBS] MeteoAlerts calling URL %s\n", url);

    WiFiClientSecure awc;
    awc.setInsecure();
    HTTPClient ahttp;
    ahttp.setConnectTimeout(2500);
    ahttp.setTimeout(3500);
    if (!ahttp.begin(awc, url)) {
        LOG_DEBUG("[LittleBBS] MeteoAlerts begin() failed");
        return String(unknownSummary);
    }
    ahttp.addHeader("User-Agent", "LittleBBS/1.0");

    int acode = ahttp.GET();
    if (acode < 0) {
        LOG_WARN("[LittleBBS] MeteoAlerts - GET failed (%d): %s", acode, ahttp.errorToString(acode).c_str());
    }
    LOG_DEBUG("[LittleBBS] MeteoAlerts - HTTP code=%d", acode);
    if (acode != 200) {
        ahttp.end();
        awc.stop();
        return String(unknownSummary);
    }

    String body = ahttp.getString();
    LOG_DEBUG("[LittleBBS] MeteoAlerts - got response len=%u", static_cast<unsigned>(body.length()));
    ahttp.end();
    awc.stop();

    if (body.length() == 0) {
        return String(unknownSummary);
    }

    static char sbuf[3072];
    strncpy(sbuf, body.c_str(), sizeof(sbuf) - 1);
    sbuf[sizeof(sbuf) - 1] = '\0';

    auto skipWs = [](const char *p) {
        while (p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
            p++;
        }
        return p;
    };

    auto parseJsonBool = [&](const char *jsonIn, const char *key, bool &value) {
        if (!jsonIn || !key) {
            return false;
        }
        const char *kp = strstr(jsonIn, key);
        if (!kp) {
            return false;
        }
        const char *cp = strchr(kp + strlen(key), ':');
        if (!cp) {
            return false;
        }
        cp = skipWs(cp + 1);
        if (!cp) {
            return false;
        }
        if (strncmp(cp, "true", 4) == 0) {
            value = true;
            return true;
        }
        if (strncmp(cp, "false", 5) == 0) {
            value = false;
            return true;
        }
        return false;
    };

    auto parseJsonString = [&](const char *jsonIn, const char *key, char *out, size_t outLen) {
        if (!jsonIn || !key || !out || outLen == 0) {
            return false;
        }
        const char *kp = strstr(jsonIn, key);
        if (!kp) {
            return false;
        }
        const char *cp = strchr(kp + strlen(key), ':');
        if (!cp) {
            return false;
        }
        cp = skipWs(cp + 1);
        if (!cp || *cp != '"') {
            return false;
        }
        cp++;

        size_t i = 0;
        while (*cp && *cp != '"' && i < outLen - 1) {
            out[i++] = *cp++;
        }
        out[i] = '\0';
        return i > 0;
    };

    auto toLowerAscii = [](char *s) {
        if (!s) {
            return;
        }
        for (size_t i = 0; s[i] != '\0'; ++i) {
            s[i] = static_cast<char>(tolower(static_cast<unsigned char>(s[i])));
        }
    };

    bool success = false;
    if (!parseJsonBool(sbuf, "\"success\"", success) || !success) {
        LOG_DEBUG("[LittleBBS] MeteoAlerts - API reports success=false or missing success field");
        return String(unknownSummary);
    }

    const char *dataPos = strstr(sbuf, "\"data\"");
    if (!dataPos) {
        LOG_DEBUG("[LittleBBS] MeteoAlerts - missing data object");
        return String(unknownSummary);
    }

    auto extractColorForDay = [&](const char *dayKey, char *out, size_t outLen) {
        if (!dayKey || !out || outLen == 0) {
            return false;
        }
        const char *dayPos = strstr(dataPos, dayKey);
        if (!dayPos) {
            return false;
        }
        const char *allertaPos = strstr(dayPos, "\"allerta\"");
        if (!allertaPos) {
            return false;
        }
        return parseJsonString(allertaPos, "\"colore\"", out, outLen);
    };

    char oggiColor[16] = {0};
    char domaniColor[16] = {0};
    const bool haveOggi = extractColorForDay("\"oggi\"", oggiColor, sizeof(oggiColor));
    const bool haveDomani = extractColorForDay("\"domani\"", domaniColor, sizeof(domaniColor));

    if (!haveOggi && !haveDomani) {
        LOG_DEBUG("[LittleBBS] MeteoAlerts - missing oggi/domani colors");
        return String(unknownSummary);
    }

    toLowerAscii(oggiColor);
    toLowerAscii(domaniColor);

    const char *todayValue = haveOggi ? oggiColor : "unknown";
    const char *tomorrowValue = haveDomani ? domaniColor : "unknown";

    char result[80];
    snprintf(result, sizeof(result), "oggi - %s | domani - %s", todayValue, tomorrowValue);

    LOG_DEBUG("[LittleBBS] MeteoAlerts - city='%s' oggi='%s' domani='%s'", city, todayValue, tomorrowValue);

    return String(result);
}

// Static functions

// This function comes as-is from https://github.com/GoatsAndMonkeys/TinyBBS
static float jsonArrayNth(const char *tag, int n)
{
    if (!tag)
        return -999.0f;
    const char *p = strchr(tag, '[');
    if (!p)
        return -999.0f;
    p++;
    for (int i = 0; i < n; i++) {
        while (*p && *p != ',' && *p != ']')
            p++;
        if (*p != ',')
            return -999.0f;
        p++;
    }
    return atof(p);
}

// WMO weather code to short description
static const char *wmoToText(int code)
{
    if (code == 0)
        return "Clear";
    if (code <= 2)
        return "Partly Cloudy";
    if (code <= 3)
        return "Overcast";
    if (code <= 48)
        return "Foggy";
    if (code <= 55)
        return "Drizzle";
    if (code <= 65)
        return "Rain";
    if (code <= 77)
        return "Snow";
    if (code <= 82)
        return "Showers";
    if (code <= 86)
        return "Snow Showers";
    return "Stormy";
}

// WMO weather code to emoji (UTF-8, BMP only — 3-byte max)
// This function comes as-is from https://github.com/GoatsAndMonkeys/TinyBBS
static const char *wmoToEmoji(int code)
{
    if (code == 0)
        return "\xE2\x98\x80"; // ☀
    if (code <= 2)
        return "\xE2\x9B\x85"; // ⛅
    if (code <= 3)
        return "\xE2\x98\x81"; // ☁
    if (code <= 48)
        return "\xE2\x98\x81"; // ☁ (fog → cloudy)
    if (code <= 55)
        return "\xE2\x98\x94"; // ☔
    if (code <= 65)
        return "\xE2\x98\x94"; // ☔
    if (code <= 77)
        return "\xE2\x9D\x84"; // ❄
    if (code <= 82)
        return "\xE2\x98\x94"; // ☔
    if (code <= 86)
        return "\xE2\x9D\x84"; // ❄
    return "\xE2\x9B\x88";     // ⛈
}

#endif