//
// Copyright (C) 2022 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <sys/types.h>
#include <unistd.h>

#include "cuttlefish/common/libs/utils/result_matchers.h"
#include "cuttlefish/host/commands/cvd/cli/selector/selector_common_parser.h"
#include "cuttlefish/host/commands/cvd/cli/selector/start_selector_parser.h"
#include "cuttlefish/host/commands/cvd/cli/selector/parser_names_helper.h"

namespace cuttlefish {
namespace selector {

TEST_P(ValidNamesTest, ValidInputs) {
  auto selector_options_res = ParseCommonSelectorArguments(selector_args_);
  ASSERT_THAT(selector_options_res, IsOk());

  auto parser = StartSelectorParser::ConductSelectFlagsParser(
      *selector_options_res, cvd_common::Args{}, cvd_common::Envs{});

  ASSERT_THAT(parser, IsOk());
}

/**
 * Note that invalid inputs must be tested at the InstanceDatabase level
 */
TEST_P(ValidNamesTest, FieldsNoSubstring) {
  auto selector_options_res = ParseCommonSelectorArguments(selector_args_);
  ASSERT_THAT(selector_options_res, IsOk());

  auto parser = StartSelectorParser::ConductSelectFlagsParser(
      *selector_options_res, cvd_common::Args{}, cvd_common::Envs{});

  ASSERT_THAT(parser, IsOk());
  ASSERT_EQ(parser->GroupName(), expected_output_.group_name);
  ASSERT_EQ(parser->PerInstanceNames(), expected_output_.per_instance_names);
}

INSTANTIATE_TEST_SUITE_P(
    CvdParser, ValidNamesTest,
    testing::Values(
        InputOutput{.input = "--group_name=cf",
                    .expected = ExpectedOutput{.group_name = "cf"}},
        InputOutput{.input = "--instance_name=cvd,cf",
                    .expected = ExpectedOutput{.per_instance_names =
                                                   std::vector<std::string>{
                                                       "cvd", "cf"}}},
        InputOutput{.input = "--instance_name=09-1,tv-2 --group_name cf",
                    .expected = ExpectedOutput{.group_name = "cf",
                                               .per_instance_names =
                                                   std::vector<std::string>{
                                                       "09-1", "tv-2"}}},
        InputOutput{
            .input = "--group_name=cf --instance_name 09",
            .expected = ExpectedOutput{.group_name = "cf",
                                       .per_instance_names =
                                           std::vector<std::string>{"09"}}},
        InputOutput{.input = "--group_name=my_cool --instance_name=phone-1,tv",
                    .expected = ExpectedOutput{.group_name = "my_cool",
                                               .per_instance_names =
                                                   std::vector<std::string>{
                                                       "phone-1", "tv"}}},
        InputOutput{
            .input = "--instance_name=my-cool",
            .expected = ExpectedOutput{
                .per_instance_names = std::vector<std::string>{"my-cool"}}}));

TEST_P(InvalidNamesTest, InvalidInputs) {
  auto selector_options_res = ParseCommonSelectorArguments(selector_args_);
  ASSERT_FALSE(selector_options_res.ok());
}

INSTANTIATE_TEST_SUITE_P(CvdParser, InvalidNamesTest,
                         testing::Values("--group_name", "--group_name=?34",
                                         "--group_name=ab-cd",
                                         "--group_name=3a", "--instance_name",
                                         "--instance_name=*7a"));

}  // namespace selector
}  // namespace cuttlefish
