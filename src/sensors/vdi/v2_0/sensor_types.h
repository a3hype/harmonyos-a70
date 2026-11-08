/*
 * Minimal stand-in for the generated ohos.hdi.sensor.v2_0 SensorTypes header
 * (drivers_interface/sensor/v2_0/SensorTypes.idl, Apache-2.0). Only the event
 * struct is needed by isensor_callback_vdi.h.
 */
#ifndef OHOS_HDI_SENSOR_V2_0_SENSORTYPES_H
#define OHOS_HDI_SENSOR_V2_0_SENSORTYPES_H
#include <cstdint>
#include <string>
#include <vector>
namespace OHOS {
namespace HDI {
namespace Sensor {
namespace V2_0 {
struct HdfSensorEvents {
    int32_t sensorId;
    int32_t version;
    int64_t timestamp;
    uint32_t option;
    int32_t mode;
    std::vector<uint8_t> data;
    uint32_t dataLen;
};
} // V2_0
} // Sensor
} // HDI
} // OHOS
#endif
