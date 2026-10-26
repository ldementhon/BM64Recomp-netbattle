#pragma once
#include <string>

namespace recomp {
struct ControllerIdentity {
    std::string guid, serial, path, name;
    bool operator==(const ControllerIdentity&) const = default;

    bool matches(const ControllerIdentity& candidate) const {
        if (guid != candidate.guid) return false;
        if (!serial.empty() && serial != candidate.serial) return false;
        if (!path.empty() && path != candidate.path) return false;
        // Some SDL backends expose neither serial nor path. Callers must still
        // require exactly one match; enumeration order cannot identify a pad.
        return !serial.empty() || !path.empty() || name == candidate.name;
    }
};
struct OnlineController {
    int port;
    ControllerIdentity device;
};
}
