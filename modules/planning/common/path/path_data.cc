/******************************************************************************
 * Copyright 2017 The Apollo Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/

/**
 * @file path_data.cc
 **/

#include "modules/planning/common/path/path_data.h"

#include <algorithm>
#include <limits>

#include "modules/common/log.h"
#include "modules/common/util/string_util.h"
#include "modules/common/util/util.h"
#include "modules/planning/common/planning_gflags.h"
#include "modules/planning/common/planning_util.h"
#include "modules/planning/math/double.h"
#include "modules/planning/math/sl_analytic_transformation.h"

namespace apollo {
namespace planning {

using SLPoint = apollo::common::SLPoint;
using Vec2d = apollo::common::math::Vec2d;

bool PathData::set_discretized_path(const DiscretizedPath &path) {
  if (reference_line_ == nullptr) {
    AERROR << "Should NOT set discretized path when reference line is nullptr. "
              "Please set reference line first.";
    return false;
  }
  discretized_path_ = path;
  if (!CartesianToFrenet(discretized_path_, &frenet_path_)) {
    AERROR << "Fail to transfer discretized path to frenet path.";
    return false;
  }
  DCHECK_EQ(discretized_path_.NumOfPoints(), frenet_path_.points().size());
  return true;
}

bool PathData::set_frenet_path(const FrenetFramePath &frenet_path) {
  if (reference_line_ == nullptr) {
    AERROR << "Should NOT set frenet path when reference line is nullptr. "
              "Please set reference line first.";
    return false;
  }
  frenet_path_ = frenet_path;
  if (!FrenetToCartesian(frenet_path_, &discretized_path_)) {
    AERROR << "Fail to transfer frenet path to discretized path.";
    return false;
  }
  DCHECK_EQ(discretized_path_.NumOfPoints(), frenet_path_.points().size());
  return true;
}

const DiscretizedPath &PathData::discretized_path() const {
  return discretized_path_;
}

const FrenetFramePath &PathData::frenet_frame_path() const {
  return frenet_path_;
}

void PathData::set_reference_line(const ReferenceLine *reference_line) {
  Clear();
  reference_line_ = reference_line;
}

bool PathData::get_path_point_with_path_s(
    const double s, common::PathPoint *const path_point) const {
  *path_point = discretized_path_.EvaluateUsingLinearApproximation(s);
  return true;
}

bool PathData::get_path_point_with_ref_s(
    const double ref_s, common::PathPoint *const path_point) const {
  DCHECK_NOTNULL(reference_line_);
  DCHECK_NOTNULL(path_point);
  DCHECK_EQ(discretized_path_.path_points().size(), frenet_path_.points().size());

  uint32_t index = 0;
  const double kDistanceEpsilon = 1e-3;
  double shortest_distance = std::numeric_limits<double>::max();
  for (uint32_t i = 0; i < frenet_path_.points().size(); ++i) {
    const double curr_distance =
        std::fabs(ref_s - frenet_path_.points().at(i).s());
    if (curr_distance < kDistanceEpsilon) {
      path_point->CopyFrom(discretized_path_.PathPointAt(i));
      return true;
    }
    if (curr_distance < shortest_distance) {
      index = i;
      shortest_distance = curr_distance;
    }
  }
  path_point->CopyFrom(discretized_path_.PathPointAt(index));

  return true;
}

void PathData::Clear() {
  discretized_path_ = DiscretizedPath();
  frenet_path_ = FrenetFramePath();
  reference_line_ = nullptr;
}

std::string PathData::DebugString() const {
  const auto &path_points = discretized_path_.path_points();
  const auto limit =
      std::min(path_points.size(),
               static_cast<size_t>(FLAGS_trajectory_point_num_for_debug));

  return apollo::common::util::StrCat(
      "[\n", apollo::common::util::PrintDebugStringIter(
                 path_points.begin(), path_points.begin() + limit, ",\n"),
      "]\n");
}

bool PathData::FrenetToCartesian(const FrenetFramePath &frenet_path,
                                 DiscretizedPath *const discretized_path) {
  DCHECK_NOTNULL(discretized_path);
  std::vector<common::PathPoint> path_points;
  for (const common::FrenetFramePoint &frenet_point : frenet_path.points()) {
    common::SLPoint sl_point;
    common::math::Vec2d cartesian_point;
    sl_point.set_s(frenet_point.s());
    sl_point.set_l(frenet_point.l());
    if (!reference_line_->get_point_in_cartesian_frame(sl_point,
                                                       &cartesian_point)) {
      AERROR << "Fail to convert sl point to xy point";
      return false;
    }
    ReferencePoint ref_point =
        reference_line_->get_reference_point(frenet_point.s());
    double theta = SLAnalyticTransformation::calculate_theta(
        ref_point.heading(), ref_point.kappa(), frenet_point.l(),
        frenet_point.dl());
    double kappa = SLAnalyticTransformation::calculate_kappa(
        ref_point.kappa(), ref_point.dkappa(), frenet_point.l(),
        frenet_point.dl(), frenet_point.ddl());

    common::PathPoint path_point = common::util::MakePathPoint(
        cartesian_point.x(), cartesian_point.y(), 0.0, theta, kappa, 0.0, 0.0);

    if (path_points.empty()) {
      path_point.set_s(0.0);
    } else {
      common::math::Vec2d last(path_points.back().x(), path_points.back().y());
      common::math::Vec2d current(path_point.x(), path_point.y());
      double distance = (last - current).Length();
      path_point.set_s(path_points.back().s() + distance);
    }
    path_points.push_back(std::move(path_point));
  }
  *discretized_path = DiscretizedPath(path_points);

  return true;
}

bool PathData::CartesianToFrenet(const DiscretizedPath &discretized_path,
                                 FrenetFramePath *const frenet_path) {
  DCHECK_NOTNULL(frenet_path);
  std::vector<common::FrenetFramePoint> frenet_frame_points;

  for (const auto &path_point : discretized_path.path_points()) {
    SLPoint sl_point;
    if (!reference_line_->get_point_in_frenet_frame(
            Vec2d(path_point.x(), path_point.y()), &sl_point)) {
      AERROR << "Fail to transfer cartesian point to frenet point.";
      return false;
    }
    common::FrenetFramePoint frenet_point;
    // NOTICE: does not set dl and ddl here. Add if needed.
    frenet_point.set_s(sl_point.s());
    frenet_point.set_l(sl_point.l());
    frenet_frame_points.push_back(std::move(frenet_point));
  }
  *frenet_path = FrenetFramePath(frenet_frame_points);
  return true;
}

}  // namespace planning
}  // namespace apollo
