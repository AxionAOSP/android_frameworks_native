/*
 * Copyright 2025 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <com_android_input_flags.h>
#include <gtest/gtest.h>
#include <input/DisplayTopologyGraph.h>

#include <string>
#include <string_view>
#include <tuple>

#include "ScopedFlagOverride.h"

namespace android {

namespace {

constexpr ui::LogicalDisplayId DISPLAY_ID_1{1};
constexpr ui::LogicalDisplayId DISPLAY_ID_2{2};
constexpr int DENSITY_MEDIUM = 160;

} // namespace

using DisplayTopologyAdjacentDisplayMap =
        std::unordered_map<ui::LogicalDisplayId, std::vector<DisplayTopologyAdjacentDisplay>>;
using DisplayTopologyDisplaysDensityMapVector = std::unordered_map<ui::LogicalDisplayId, int>;
using DisplayTopologyGraphTestFixtureParam =
        std::tuple<std::string_view /*name*/, ui::LogicalDisplayId /*primaryDisplayId*/,
                   DisplayTopologyAdjacentDisplayMap, DisplayTopologyDisplaysDensityMapVector,
                   bool /*isValid*/>;

class DisplayTopologyGraphTestFixture
      : public testing::Test,
        public testing::WithParamInterface<DisplayTopologyGraphTestFixtureParam> {};

TEST_P(DisplayTopologyGraphTestFixture, DisplayTopologyGraphTest) {
    SCOPED_FLAG_OVERRIDE(enable_display_topology_validation, true);
    auto [_, primaryDisplayId, graph, displaysDensity, isValid] = GetParam();
    auto result = DisplayTopologyGraph::create(primaryDisplayId, std::move(graph),
                                               std::move(displaysDensity));
    EXPECT_EQ(isValid, result.ok());
}

INSTANTIATE_TEST_SUITE_P(
        DisplayTopologyGraphTest, DisplayTopologyGraphTestFixture,
        testing::Values(
                std::make_tuple("InvalidPrimaryDisplay",
                                /*primaryDisplayId=*/ui::LogicalDisplayId::INVALID,
                                /*graph=*/DisplayTopologyAdjacentDisplayMap{},
                                /*displaysDensity=*/DisplayTopologyDisplaysDensityMapVector{},
                                false),
                std::make_tuple("PrimaryDisplayNotInGraph",
                                /*primaryDisplayId=*/DISPLAY_ID_1,
                                /*graph=*/DisplayTopologyAdjacentDisplayMap{},
                                /*displaysDensity=*/DisplayTopologyDisplaysDensityMapVector{},
                                false),
                std::make_tuple("DisplayDensityMissing",
                                /*primaryDisplayId=*/DISPLAY_ID_1,
                                /*graph=*/DisplayTopologyAdjacentDisplayMap{{DISPLAY_ID_1, {}}},
                                /*displaysDensity=*/DisplayTopologyDisplaysDensityMapVector{},
                                false),
                std::make_tuple("ValidSingleDisplayTopology",
                                /*primaryDisplayId=*/DISPLAY_ID_1,
                                /*graph=*/DisplayTopologyAdjacentDisplayMap{{DISPLAY_ID_1, {}}},
                                /*displaysDensity=*/
                                DisplayTopologyDisplaysDensityMapVector{
                                        {DISPLAY_ID_1, DENSITY_MEDIUM}},
                                true),
                std::make_tuple(
                        "MissingReverseEdge",
                        /*primaryDisplayId=*/DISPLAY_ID_1,
                        /*graph=*/
                        DisplayTopologyAdjacentDisplayMap{
                                {DISPLAY_ID_1, {{DISPLAY_ID_2, DisplayTopologyPosition::TOP, 0}}}},
                        /*displaysDensity=*/
                        DisplayTopologyDisplaysDensityMapVector{{DISPLAY_ID_1, DENSITY_MEDIUM},
                                                                {DISPLAY_ID_2, DENSITY_MEDIUM}},
                        false),
                std::make_tuple(
                        "IncorrectReverseEdgeDirection",
                        /*primaryDisplayId=*/DISPLAY_ID_1,
                        /*graph=*/
                        DisplayTopologyAdjacentDisplayMap{{DISPLAY_ID_1,
                                                           {{DISPLAY_ID_2,
                                                             DisplayTopologyPosition::TOP, 0}}},
                                                          {DISPLAY_ID_2,
                                                           {{DISPLAY_ID_1,
                                                             DisplayTopologyPosition::TOP, 0}}}},
                        /*displaysDensity=*/
                        DisplayTopologyDisplaysDensityMapVector{{DISPLAY_ID_1, DENSITY_MEDIUM},
                                                                {DISPLAY_ID_2, DENSITY_MEDIUM}},
                        false),
                std::make_tuple(
                        "IncorrectReverseEdgeOffset",
                        /*primaryDisplayId=*/DISPLAY_ID_1,
                        /*graph=*/
                        DisplayTopologyAdjacentDisplayMap{{DISPLAY_ID_1,
                                                           {{DISPLAY_ID_2,
                                                             DisplayTopologyPosition::TOP, 10}}},
                                                          {DISPLAY_ID_2,
                                                           {{DISPLAY_ID_1,
                                                             DisplayTopologyPosition::BOTTOM,
                                                             20}}}},
                        /*displaysDensity=*/
                        DisplayTopologyDisplaysDensityMapVector{{DISPLAY_ID_1, DENSITY_MEDIUM},
                                                                {DISPLAY_ID_2, DENSITY_MEDIUM}},
                        false),
                std::make_tuple(
                        "ValidMultiDisplayTopology",
                        /*primaryDisplayId=*/DISPLAY_ID_1,
                        /*graph=*/
                        DisplayTopologyAdjacentDisplayMap{{DISPLAY_ID_1,
                                                           {{DISPLAY_ID_2,
                                                             DisplayTopologyPosition::TOP, 10}}},
                                                          {DISPLAY_ID_2,
                                                           {{DISPLAY_ID_1,
                                                             DisplayTopologyPosition::BOTTOM,
                                                             -10}}}},
                        /*displaysDensity=*/
                        DisplayTopologyDisplaysDensityMapVector{{DISPLAY_ID_1, DENSITY_MEDIUM},
                                                                {DISPLAY_ID_2, DENSITY_MEDIUM}},
                        true)),
        [](const testing::TestParamInfo<DisplayTopologyGraphTestFixtureParam>& p) {
            return std::string{std::get<0>(p.param)};
        });

} // namespace android
