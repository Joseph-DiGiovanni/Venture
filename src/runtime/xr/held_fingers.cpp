#include "xr/held_fingers.h"
#include "common/log.h"
#include "core/wait.h"
#include "xr/clock.h"
#include "xr/host_xr.h"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <utility>
namespace venture::xr {
namespace {
struct Held {
  XrSession session{};
  bool tried = false;  // the trackers were created, or could not be
  XrHandTrackerEXT trackers[2]{};
  XrSpace space{};  // any space: only the finger's own angles are read
  bool near[2]{};
};
std::mutex held_mutex;
Held& held() {
  static Held value;
  return value;
}
void release(Held& h) {
  for (auto& tracker : h.trackers)
    if (tracker) host_call<PFN_xrDestroyHandTrackerEXT, "xrDestroyHandTrackerEXT">(std::exchange(tracker, {}));
  if (h.space) ::xrDestroySpace(std::exchange(h.space, {}));
  h = {};
}
void create(Held& h) {
  const auto& extensions = host_xr().enabled_extensions;
  if (!extensions.contains("XR_EXT_hand_tracking") || !extensions.contains("XR_EXT_hand_tracking_data_source"))
    return;
  const XrReferenceSpaceCreateInfo view{XR_TYPE_REFERENCE_SPACE_CREATE_INFO, nullptr, XR_REFERENCE_SPACE_TYPE_VIEW,
                                        {{0, 0, 0, 1}, {0, 0, 0}}};
  if (XR_FAILED(::xrCreateReferenceSpace(h.session, &view, &h.space))) {
    h.space = {};
    return;
  }
  XrHandTrackingDataSourceEXT held_source = XR_HAND_TRACKING_DATA_SOURCE_CONTROLLER_EXT;
  const XrHandTrackingDataSourceInfoEXT source{XR_TYPE_HAND_TRACKING_DATA_SOURCE_INFO_EXT, nullptr, 1, &held_source};
  for (int hand = 0; hand < 2; ++hand) {
    XrHandTrackerCreateInfoEXT ci{XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT, &source};
    ci.hand = hand ? XR_HAND_RIGHT_EXT : XR_HAND_LEFT_EXT;
    ci.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
    if (XR_FAILED((host_call<PFN_xrCreateHandTrackerEXT, "xrCreateHandTrackerEXT">(h.session, &ci, &h.trackers[hand]))))
      h.trackers[hand] = {};
  }
  if (h.trackers[0] || h.trackers[1]) log::info("xr", "Index finger proximity from the host's controller skeletons");
}
}  // namespace
std::optional<float> index_bend(const XrHandJointLocationEXT* joints) {
  constexpr int metacarpal = XR_HAND_JOINT_INDEX_METACARPAL_EXT, proximal = XR_HAND_JOINT_INDEX_PROXIMAL_EXT,
                distal = XR_HAND_JOINT_INDEX_DISTAL_EXT, tip = XR_HAND_JOINT_INDEX_TIP_EXT;
  for (const int j : {metacarpal, proximal, distal, tip})
    if (!(joints[j].locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) return std::nullopt;
  auto bone = [&](int from, int to) {
    const auto& a = joints[from].pose.position;
    const auto& b = joints[to].pose.position;
    return XrVector3f{b.x - a.x, b.y - a.y, b.z - a.z};
  };
  const auto first = bone(metacarpal, proximal), last = bone(distal, tip);
  const float lengths = std::hypot(first.x, first.y, first.z) * std::hypot(last.x, last.y, last.z);
  if (!(lengths > 0) || !std::isfinite(lengths)) return std::nullopt;
  const float cosine = (first.x * last.x + first.y * last.y + first.z * last.z) / lengths;
  return std::acos(std::clamp(cosine, -1.f, 1.f)) * 57.29578f;
}
std::optional<bool> held_index_near(XrSession session, int hand, int pointing) {
  auto& h = held();
  std::lock_guard lock(held_mutex);
  if (h.session != session) {
    h = {};  // forget_held_fingers ended the previous session's objects
    h.session = session;
  }
  if (!std::exchange(h.tried, true)) create(h);
  const auto tracker = h.trackers[hand];
  XrTime now = 0;
  if (!tracker || XR_FAILED(monotonic_to_time(venture::monotonic_ns(), &now))) return std::nullopt;
  XrHandJointLocationEXT joints[XR_HAND_JOINT_COUNT_EXT]{};
  XrHandTrackingDataSourceStateEXT source{XR_TYPE_HAND_TRACKING_DATA_SOURCE_STATE_EXT};
  XrHandJointLocationsEXT located{XR_TYPE_HAND_JOINT_LOCATIONS_EXT, &source, XR_FALSE, XR_HAND_JOINT_COUNT_EXT, joints};
  const XrHandJointsLocateInfoEXT info{XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT, nullptr, h.space, now};
  const auto bend =
      XR_SUCCEEDED((host_call<PFN_xrLocateHandJointsEXT, "xrLocateHandJointsEXT">(tracker, &info, &located))) &&
              located.isActive && source.isActive && source.dataSource == XR_HAND_TRACKING_DATA_SOURCE_CONTROLLER_EXT
          ? index_bend(joints)
          : std::nullopt;
  if (!bend) return h.near[hand] = false, std::nullopt;
  return h.near[hand] = bend_near(*bend, h.near[hand], point_bend(pointing));
}
void forget_held_fingers(XrSession session) {
  auto& h = held();
  std::lock_guard lock(held_mutex);
  if (h.session == session) release(h);
}
}  // namespace venture::xr
