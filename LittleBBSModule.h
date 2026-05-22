#pragma once
#include "configuration.h"
#if !MESHTASTIC_EXCLUDE_LITTLEBBS
// #include "concurrency/OSThread.h"
// #include "graphics/Screen.h"
#include "SinglePortModule.h"
#include "mesh/generated/meshtastic/mesh.pb.h"

class LittleBBSModule : public SinglePortModule, private concurrency::OSThread
{
  public:
    LittleBBSModule();
    void setup() override;
    bool wantPacket(const meshtastic_MeshPacket *p) override;
    ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;
    int32_t runOnce() override;

  protected:
    void sendDm(const meshtastic_MeshPacket &rx, const char *text);
    void sendMainMenu(const meshtastic_MeshPacket &mp);
    void getRemoteNodeCoordinates(const meshtastic_MeshPacket &mp, float &lat, float &lon);
    bool reverseGeocode(float lat, float lon, char *city, size_t cityLen, char *country, size_t countryLen);
    bool getWeatherForecast(char *buf, size_t bufLen, float lat, float lon);
};
extern LittleBBSModule *littleBBSModule;
#endif
