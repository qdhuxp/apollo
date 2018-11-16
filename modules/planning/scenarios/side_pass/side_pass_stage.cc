/******************************************************************************
 * Copyright 2018 The Apollo Authors. All Rights Reserved.
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
 * @file
 **/

#include "modules/planning/scenarios/side_pass/side_pass_stage.h"

#include <algorithm>
#include <vector>

#include "modules/common/proto/pnc_point.pb.h"
#include "modules/planning/common/frame.h"
#include "modules/planning/common/planning_gflags.h"
#include "modules/planning/common/speed_profile_generator.h"

namespace apollo {
namespace planning {
namespace scenario {
namespace side_pass {

using apollo::common::TrajectoryPoint;
using apollo::common::math::Vec2d;

constexpr double kExtraMarginforStopOnWaitPointStage = 3.0;

/*
 * @brief:
 * STAGE: SidePassBackup
 */
Stage::StageStatus SidePassBackup::Process(
    const TrajectoryPoint& planning_start_point, Frame* frame) {
  // check the status of side pass scenario
  const SLBoundary& adc_sl_boundary =
      frame->reference_line_info().front().AdcSlBoundary();
  const PathDecision& path_decision =
      frame->reference_line_info().front().path_decision();

  bool has_blocking_obstacle = false;
  for (const auto* obstacle : path_decision.obstacles().Items()) {
    if (obstacle->IsVirtual() || !obstacle->IsStatic()) {
      continue;
    }
    CHECK(obstacle->IsStatic());
    if (obstacle->speed() >
        GetContext()->scenario_config_.block_obstacle_min_speed()) {
      continue;
    }
    if (obstacle->PerceptionSLBoundary().start_s() <=
        adc_sl_boundary.end_s()) {  // such vehicles are behind the ego car.
      continue;
    }
    constexpr double kAdcDistanceThreshold = 15.0;  // unit: m
    if (obstacle->PerceptionSLBoundary().start_s() >
        adc_sl_boundary.end_s() +
            kAdcDistanceThreshold) {  // vehicles are far away
      continue;
    }

    // check l
    constexpr double kLBufferThreshold = 0.3;  // unit: m
    const auto& reference_line =
        frame->reference_line_info().front().reference_line();
    double lane_left_width = 0.0;
    double lane_right_width = 0.0;
    reference_line.GetLaneWidth(obstacle->PerceptionSLBoundary().start_s(),
                                &lane_left_width, &lane_right_width);
    double driving_width =
        std::max(lane_left_width - obstacle->PerceptionSLBoundary().end_l(),
                 lane_right_width + obstacle->PerceptionSLBoundary().start_l());
    driving_width =
        std::min(lane_left_width + lane_right_width, driving_width) -
        FLAGS_static_decision_nudge_l_buffer;
    ADEBUG << "driving_width[" << driving_width << "]";
    if (driving_width > kLBufferThreshold) {
      continue;
    }

    has_blocking_obstacle = true;
    break;
  }

  if (!has_blocking_obstacle) {
    next_stage_ = ScenarioConfig::NO_STAGE;
    return Stage::FINISHED;
  }

  // do path planning
  bool plan_ok = PlanningOnReferenceLine(planning_start_point, frame);
  if (!plan_ok) {
    AERROR << "Stage " << Name() << " error: "
           << "planning on reference line failed.";
    return Stage::ERROR;
  }
  return Stage::RUNNING;
}

/*
 * @brief:
 * STAGE: SidePassApproachObstacle
 */
Stage::StageStatus SidePassApproachObstacle::Process(
    const TrajectoryPoint& planning_start_point, Frame* frame) {
  // check the status of side pass scenario
  const SLBoundary& adc_sl_boundary =
      frame->reference_line_info().front().AdcSlBoundary();
  const PathDecision& path_decision =
      frame->reference_line_info().front().path_decision();

  bool has_blocking_obstacle = false;
  for (const auto* obstacle : path_decision.obstacles().Items()) {
    if (obstacle->IsVirtual() || !obstacle->IsStatic()) {
      continue;
    }
    CHECK(obstacle->IsStatic());
    if (obstacle->speed() >
        GetContext()->scenario_config_.block_obstacle_min_speed()) {
      continue;
    }
    if (obstacle->PerceptionSLBoundary().start_s() <=
        adc_sl_boundary.end_s()) {  // such vehicles are behind the ego car.
      continue;
    }
    constexpr double kAdcDistanceThreshold = 15.0;  // unit: m
    if (obstacle->PerceptionSLBoundary().start_s() >
        adc_sl_boundary.end_s() +
            kAdcDistanceThreshold) {  // vehicles are far away
      continue;
    }
    if (obstacle->PerceptionSLBoundary().start_l() > 1.0 ||
        obstacle->PerceptionSLBoundary().end_l() < -1.0) {
      continue;
    }
    has_blocking_obstacle = true;
  }
  if (!has_blocking_obstacle) {
    next_stage_ = ScenarioConfig::NO_STAGE;
    return Stage::FINISHED;
  }
  // do path planning
  bool plan_ok = PlanningOnReferenceLine(planning_start_point, frame);
  if (!plan_ok) {
    AERROR << "Stage " << Name() << " error: "
           << "planning on reference line failed.";
    return Stage::ERROR;
  }

  const ReferenceLineInfo& reference_line_info =
      frame->reference_line_info().front();
  double adc_velocity = frame->vehicle_state().linear_velocity();
  double adc_front_edge_s = reference_line_info.AdcSlBoundary().end_s();

  double front_obstacle_distance = 1000;
  for (const auto* obstacle : path_decision.obstacles().Items()) {
    if (obstacle->IsVirtual()) {
      continue;
    }

    bool is_on_road = reference_line_info.reference_line().HasOverlap(
        obstacle->PerceptionBoundingBox());
    if (!is_on_road) {
      continue;
    }

    const auto& obstacle_sl = obstacle->PerceptionSLBoundary();
    if (obstacle_sl.end_s() <= reference_line_info.AdcSlBoundary().start_s()) {
      continue;
    }

    double distance = obstacle_sl.start_s() - adc_front_edge_s;
    if (distance < front_obstacle_distance) {
      front_obstacle_distance = distance;
    }
  }

  if ((front_obstacle_distance) < 0) {
    AERROR << "Stage " << Name() << " error: "
           << "front obstacle has wrong position.";
    return Stage::ERROR;
  }

  double max_stop_velocity =
      GetContext()->scenario_config_.approach_obstacle_max_stop_speed();
  double min_stop_obstacle_distance =
      GetContext()->scenario_config_.approach_obstacle_min_stop_distance();

  if (adc_velocity < max_stop_velocity &&
      front_obstacle_distance > min_stop_obstacle_distance) {
    next_stage_ = ScenarioConfig::SIDE_PASS_GENERATE_PATH;
    return Stage::FINISHED;
  }

  return Stage::RUNNING;
}

/*
 * @brief:
 * STAGE: SidePassGeneratePath
 */
Stage::StageStatus SidePassGeneratePath::Process(
    const TrajectoryPoint& planning_start_point, Frame* frame) {
  if (!PlanningOnReferenceLine(planning_start_point, frame)) {
    AERROR << "Fail to plan on reference_line.";
    next_stage_ = ScenarioConfig::SIDE_PASS_BACKUP;
    return Stage::FINISHED;
  }
  GetContext()->path_data_ = frame->reference_line_info().front().path_data();
  if (frame->reference_line_info().front().trajectory().NumOfPoints() > 0) {
    next_stage_ = ScenarioConfig::SIDE_PASS_STOP_ON_WAITPOINT;
    return Stage::FINISHED;
  }
  return Stage::RUNNING;
}

/*
 * @brief:
 * STAGE: SidePassDetectSafety
 */

Stage::StageStatus SidePassDetectSafety::Process(
    const TrajectoryPoint& planning_start_point, Frame* frame) {
  const auto& reference_line_info = frame->reference_line_info().front();
  bool update_success = GetContext()->path_data_.UpdateFrenetFramePath(
      &reference_line_info.reference_line());
  if (!update_success) {
    AERROR << "Fail to update path_data.";
    return Stage::ERROR;
  }

  const auto adc_frenet_frame_point_ =
      reference_line_info.reference_line().GetFrenetPoint(
          frame->PlanningStartPoint().path_point());

  bool trim_success = GetContext()->path_data_.LeftTrimWithRefS(
      adc_frenet_frame_point_.s(), adc_frenet_frame_point_.l());
  if (!trim_success) {
    AERROR << "Fail to trim path_data. adc_frenet_frame_point: "
           << adc_frenet_frame_point_.ShortDebugString();
    return Stage::ERROR;
  }

  auto& rfl_info = frame->mutable_reference_line_info()->front();
  *(rfl_info.mutable_path_data()) = GetContext()->path_data_;

  const auto& path_points =
      rfl_info.path_data().discretized_path().path_points();
  auto* debug_path =
      rfl_info.mutable_debug()->mutable_planning_data()->add_path();

  debug_path->set_name("DpPolyPathOptimizer");
  debug_path->mutable_path_point()->CopyFrom(
      {path_points.begin(), path_points.end()});

  if (!PlanningOnReferenceLine(planning_start_point, frame)) {
    return Stage::ERROR;
  }
  bool is_safe = true;
  double adc_front_edge_s = reference_line_info.AdcSlBoundary().end_s();

  const PathDecision& path_decision =
      frame->reference_line_info().front().path_decision();
  for (const auto* obstacle : path_decision.obstacles().Items()) {
    // TODO(All): check according to neighbor lane.
    if (obstacle->IsVirtual() && obstacle->Id().substr(0, 3) == "SP_" &&
        obstacle->PerceptionSLBoundary().start_s() >= adc_front_edge_s) {
      is_safe = false;
      break;
    }
  }
  if (is_safe) {
    next_stage_ = ScenarioConfig::SIDE_PASS_PASS_OBSTACLE;
    return Stage::FINISHED;
  }
  return Stage::RUNNING;
}

/*
 * @brief:
 * STAGE: SidePassPassObstacle
 */
Stage::StageStatus SidePassPassObstacle::Process(
    const TrajectoryPoint& planning_start_point, Frame* frame) {
  const auto& reference_line_info = frame->reference_line_info().front();
  bool update_success = GetContext()->path_data_.UpdateFrenetFramePath(
      &reference_line_info.reference_line());
  if (!update_success) {
    AERROR << "Fail to update path_data.";
    return Stage::ERROR;
  }

  const auto adc_frenet_frame_point_ =
      reference_line_info.reference_line().GetFrenetPoint(
          frame->PlanningStartPoint().path_point());

  bool trim_success = GetContext()->path_data_.LeftTrimWithRefS(
      adc_frenet_frame_point_.s(), adc_frenet_frame_point_.l());
  if (!trim_success) {
    AERROR << "Fail to trim path_data. adc_frenet_frame_point: "
           << adc_frenet_frame_point_.ShortDebugString();
    return Stage::ERROR;
  }

  auto& rfl_info = frame->mutable_reference_line_info()->front();
  *(rfl_info.mutable_path_data()) = GetContext()->path_data_;

  const auto& path_points =
      rfl_info.path_data().discretized_path().path_points();
  auto* debug_path =
      rfl_info.mutable_debug()->mutable_planning_data()->add_path();

  // TODO(All):
  // Have to use DpPolyPathOptimizer to show in dreamview. Need change to
  // correct name.
  debug_path->set_name("DpPolyPathOptimizer");
  debug_path->mutable_path_point()->CopyFrom(
      {path_points.begin(), path_points.end()});

  bool plan_ok = PlanningOnReferenceLine(planning_start_point, frame);
  if (!plan_ok) {
    AERROR << "Fail to plan on reference line.";
    return Stage::ERROR;
  }

  const SLBoundary& adc_sl_boundary = reference_line_info.AdcSlBoundary();
  const auto& end_point =
      reference_line_info.path_data().discretized_path().EndPoint();
  Vec2d last_xy_point(end_point.x(), end_point.y());
  // get s of last point on path
  common::SLPoint sl_point;
  if (!reference_line_info.reference_line().XYToSL(last_xy_point, &sl_point)) {
    AERROR << "Fail to transfer cartesian point to frenet point.";
    return Stage::ERROR;
  }

  double distance_to_path_end =
      sl_point.s() - GetContext()->scenario_config_.side_pass_exit_distance();
  double adc_velocity = frame->vehicle_state().linear_velocity();
  double max_velocity_for_stop =
      GetContext()->scenario_config_.approach_obstacle_max_stop_speed();
  if (adc_sl_boundary.end_s() > distance_to_path_end ||
      adc_velocity < max_velocity_for_stop) {
    next_stage_ = ScenarioConfig::NO_STAGE;
    return Stage::FINISHED;
  }
  return Stage::RUNNING;
}

}  // namespace side_pass
}  // namespace scenario
}  // namespace planning
}  // namespace apollo
