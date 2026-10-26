#pragma once

#include "wire.hpp"

namespace batc {
template <typename Probe>
Position grounded_position(Position position, Probe&& probe,
                           std::optional<float> feedback_ground, bool feedback_received) {
    const bool on_ground = (position.flags & 1) != 0;
    // BATC can leave landing rollout marked airborne until runway exit.
    const bool near_ground = feedback_ground && position.altitude <= *feedback_ground + 50.0f;
    if (on_ground || !feedback_received || near_ground) {
        const float ground = probe(position.lat, position.lon, position.altitude);
        position.altitude = on_ground ? ground : std::max(position.altitude, ground);
    }
    return position;
}
}
