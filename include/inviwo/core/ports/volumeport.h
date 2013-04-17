#ifndef IVW_VOLUMEPORT_H
#define IVW_VOLUMEPORT_H

#include <inviwo/core/common/inviwocoredefine.h>
#include <inviwo/core/ports/dataport.h>
#include <inviwo/core/datastructures/volume.h>

namespace inviwo {

class IVW_CORE_API VolumePort : public DataPort<Volume> {

public:
    VolumePort(Port::PortDirection direction, std::string identifier);
    virtual ~VolumePort();

    void initialize();
    void deinitialize();

    uvec3 getColorCode() const { return uvec3(188,101,101); }
};

} // namespace

#endif // IVW_VOLUMEPORT_H
