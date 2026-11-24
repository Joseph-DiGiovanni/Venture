#pragma once
// The index finger on a held controller, from the host's hand skeleton for it
// (XR_EXT_hand_tracking_data_source, controller source). SteamVR builds that
// skeleton from the controller's finger sensors, and on the Steam Frame its
// index curl follows the finger's distance from the trigger, which no
// controller input reports: about 164 degrees resting on the trigger, 100 to
// 112 closing in on it, 60 to 95 lifted just off it in a relaxed point (the
// same as hovering 1 cm away) and 13 pointing straight. Its thumb follows the
// touch sensors only, so the thumb is left to them.
#include "xr/api.h"
#include <optional>
namespace venture::xr {
// How far the index finger bends, in degrees: the angle between its first bone
// (metacarpal to proximal joint) and its last (distal joint to tip), from 26
// XR_EXT_hand_tracking joints. Nullopt where those joints are not located.
std::optional<float> index_bend(const XrHandJointLocationEXT* joints);
// The pointing dead zone (controls::Tuning::pointing): the finger points while
// it curls less than `percent` of the way to resting on the trigger (~164
// degrees on the Steam Frame). Venture's own (0) is 50 %, about 82 degrees: a
// relaxed point points, and a finger closing in on the trigger is near.
inline constexpr float kCurledBend = 164.f;
inline constexpr int kAutoPointing = 50;
inline float point_bend(int percent) { return kCurledBend * float(percent ? percent : kAutoPointing) / 100.f; }
// Whether a finger bent `degrees` is near the trigger, given whether it was:
// near 8 degrees past the dead zone's edge, far again only inside it, so a
// finger at the edge does not flicker.
inline bool bend_near(float degrees, bool was_near, float point = point_bend(0)) {
  return degrees >= (was_near ? point : point + 8.f);
}
// Whether the index finger of `hand` (0 left, 1 right) is near the trigger by
// the host's controller skeleton, under a pointing dead zone of `pointing` %
// (point_bend), or nullopt where the host gives none (no extension, no hand
// tracking, or no skeleton for the controller right now). The first call for a
// session creates its trackers.
std::optional<bool> held_index_near(XrSession session, int hand, int pointing);
// Before `session` is destroyed: its trackers go.
void forget_held_fingers(XrSession session);
} // namespace venture::xr
