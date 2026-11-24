#pragma once
// The index finger on a held controller, from the host's hand skeleton for it
// (XR_EXT_hand_tracking_data_source, controller source). SteamVR builds that
// skeleton from the controller's finger sensors, and on the Steam Frame its
// index curl follows the finger's distance from the trigger, which no
// controller input reports: about 164 degrees resting on the trigger, 60 at
// 1 cm, 45 at 3 cm and 13 pointing straight. Its thumb follows the touch
// sensors only, so the thumb is left to them.
#include "xr/api.h"
#include <optional>
namespace venture::xr {
// How far the index finger bends, in degrees: the angle between its first bone
// (metacarpal to proximal joint) and its last (distal joint to tip), from 26
// XR_EXT_hand_tracking joints. Nullopt where those joints are not located.
std::optional<float> index_bend(const XrHandJointLocationEXT* joints);
// Whether a finger bent `degrees` is near the trigger, given whether it was:
// near from kNearBend up, far again only below kFarBend, so a finger at the
// edge does not flicker.
inline constexpr float kNearBend = 32.f, kFarBend = 24.f;
inline bool bend_near(float degrees, bool was_near) { return degrees >= (was_near ? kFarBend : kNearBend); }
// Whether the index finger of `hand` (0 left, 1 right) is near the trigger by
// the host's controller skeleton, or nullopt where the host gives none (no
// extension, no hand tracking, or no skeleton for the controller right now).
// The first call for a session creates its trackers.
std::optional<bool> held_index_near(XrSession session, int hand);
// Before `session` is destroyed: its trackers go.
void forget_held_fingers(XrSession session);
} // namespace venture::xr
