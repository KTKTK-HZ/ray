// Copyright 2017 The Ray Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "ray/stats/metric.h"

#include <memory>

#include "opencensus/stats/measure_registry.h"

namespace ray {

namespace stats {

namespace internal {

void RegisterAsView(opencensus::stats::ViewDescriptor view_descriptor,
                    const std::vector<opencensus::tags::TagKey> &keys) {
  // Register global keys.
  for (const auto &tag : ray::stats::StatsConfig::instance().GetGlobalTags()) {
    view_descriptor = view_descriptor.add_column(tag.first);
  }

  // Register custom keys.
  for (const auto &key : keys) {
    view_descriptor = view_descriptor.add_column(key);
  }
  opencensus::stats::View view(view_descriptor);
  view_descriptor.RegisterForExport();
}

}  // namespace internal
///
/// Stats Config
///

StatsConfig &StatsConfig::instance() {
  static StatsConfig instance;
  return instance;
}

void StatsConfig::SetGlobalTags(const TagsType &global_tags) {
  global_tags_ = global_tags;
}

const TagsType &StatsConfig::GetGlobalTags() const { return global_tags_; }

void StatsConfig::SetIsDisableStats(bool disable_stats) {
  is_stats_disabled_ = disable_stats;
}

bool StatsConfig::IsStatsDisabled() const { return is_stats_disabled_; }

void StatsConfig::SetReportInterval(const absl::Duration interval) {
  report_interval_ = interval;
}

const absl::Duration &StatsConfig::GetReportInterval() const { return report_interval_; }

void StatsConfig::SetHarvestInterval(const absl::Duration interval) {
  harvest_interval_ = interval;
}

const absl::Duration &StatsConfig::GetHarvestInterval() const {
  return harvest_interval_;
}

void StatsConfig::SetIsInitialized(bool initialized) { is_initialized_ = initialized; }

bool StatsConfig::IsInitialized() const { return is_initialized_; }

///
/// Metric
///
using MeasureDouble = opencensus::stats::Measure<double>;
Metric::Metric(const std::string &name,
               std::string description,
               std::string unit,
               const std::vector<std::string> &tag_keys)
    : name_(name),
      description_(std::move(description)),
      unit_(std::move(unit)),
      measure_(nullptr),
      name_regex_(GetMetricNameRegex()) {
  RAY_CHECK_WITH_DISPLAY(
      std::regex_match(name, Metric::name_regex_),
      "Invalid metric name: " + name +
          ". Metric names can only contain letters, numbers, _, and :. "
          "Metric names cannot start with numbers. Metric name cannot be "
          "empty.");
  for (const auto &key : tag_keys) {
    tag_keys_.push_back(opencensus::tags::TagKey::Register(key));
  }
}

const std::regex &Metric::GetMetricNameRegex() {
  const static std::regex name_regex("^[a-zA-Z_:][a-zA-Z0-9_:]*$");
  return name_regex;
}

void Metric::Record(double value, TagsType tags) {
  if (StatsConfig::instance().IsStatsDisabled()) {
    return;
  }

  if (::RayConfig::instance().experimental_enable_open_telemetry_on_core()) {
    // Register the metric if it hasn't been registered yet; otherwise, this is a no-op.
    // We defer metric registration until the first time it's recorded, rather than during
    // construction, to avoid issues with static initialization order. Specifically, our
    // internal Metric objects (see metric_defs.h) are declared as static, and
    // constructing another static object within their constructor can lead to crashes at
    // program exit due to unpredictable destruction order.
    //
    // Once these internal Metric objects are migrated to use DEFINE_stats, we can
    // safely move the registration logic to the constructor. See
    // https://github.com/ray-project/ray/issues/54538 for the backlog of Ray metric infra
    // improvements.
    //
    // This function is thread-safe.
    RegisterOpenTelemetryMetric();
    // Collect tags from both the metric-specific tags and the global tags.
    absl::flat_hash_map<std::string, std::string> open_telemetry_tags;
    std::unordered_set<std::string> tag_keys_set;
    for (const auto &tag_key : tag_keys_) {
      tag_keys_set.insert(tag_key.name());
    }
    // Insert metric-specific tags that match the expected keys.
    for (const auto &tag : tags) {
      const std::string &key = tag.first.name();
      if (tag_keys_set.count(key)) {
        open_telemetry_tags[key] = tag.second;
      }
    }
    // Add global tags, overwriting any existing tag keys.
    for (const auto &tag : StatsConfig::instance().GetGlobalTags()) {
      open_telemetry_tags[tag.first.name()] = tag.second;
    }
    OpenTelemetryMetricRecorder::GetInstance().SetMetricValue(
        name_, std::move(open_telemetry_tags), value);

    return;
  }

  absl::MutexLock lock(&registration_mutex_);
  if (measure_ == nullptr) {
    // Measure could be registered before, so we try to get it first.
    MeasureDouble registered_measure =
        opencensus::stats::MeasureRegistry::GetMeasureDoubleByName(name_);

    if (registered_measure.IsValid()) {
      measure_ = std::make_unique<MeasureDouble>(MeasureDouble(registered_measure));
    } else {
      measure_ = std::make_unique<MeasureDouble>(
          MeasureDouble::Register(name_, description_, unit_));
    }
    RegisterView();
  }

  // Do record.
  TagsType combined_tags(std::move(tags));
  combined_tags.insert(std::end(combined_tags),
                       std::begin(StatsConfig::instance().GetGlobalTags()),
                       std::end(StatsConfig::instance().GetGlobalTags()));
  opencensus::stats::Record({{*measure_, value}}, std::move(combined_tags));
}

void Metric::Record(double value,
                    std::unordered_map<std::string_view, std::string> tags) {
  TagsType tags_pair_vec;
  tags_pair_vec.reserve(tags.size());
  std::for_each(tags.begin(), tags.end(), [&tags_pair_vec](auto &tag) {
    return tags_pair_vec.emplace_back(TagKeyType::Register(tag.first),
                                      std::move(tag.second));
  });
  Record(value, std::move(tags_pair_vec));
}

void Metric::Record(double value, std::unordered_map<std::string, std::string> tags) {
  TagsType tags_pair_vec;
  tags_pair_vec.reserve(tags.size());
  std::for_each(tags.begin(), tags.end(), [&tags_pair_vec](auto &tag) {
    return tags_pair_vec.emplace_back(TagKeyType::Register(tag.first),
                                      std::move(tag.second));
  });
  Record(value, std::move(tags_pair_vec));
}

Metric::~Metric() { opencensus::stats::StatsExporter::RemoveView(name_); }

void Gauge::RegisterOpenTelemetryMetric() {
  // Register the metric in OpenTelemetry.
  OpenTelemetryMetricRecorder::GetInstance().RegisterGaugeMetric(name_, description_);
}

void Gauge::RegisterView() {
  opencensus::stats::ViewDescriptor view_descriptor =
      opencensus::stats::ViewDescriptor()
          .set_name(name_)
          .set_description(description_)
          .set_measure(name_)
          .set_aggregation(opencensus::stats::Aggregation::LastValue());
  internal::RegisterAsView(view_descriptor, tag_keys_);
}

void Histogram::RegisterOpenTelemetryMetric() {
  OpenTelemetryMetricRecorder::GetInstance().RegisterHistogramMetric(
      name_, description_, boundaries_);
}

void Histogram::RegisterView() {
  opencensus::stats::ViewDescriptor view_descriptor =
      opencensus::stats::ViewDescriptor()
          .set_name(name_)
          .set_description(description_)
          .set_measure(name_)
          .set_aggregation(opencensus::stats::Aggregation::Distribution(
              opencensus::stats::BucketBoundaries::Explicit(boundaries_)));

  internal::RegisterAsView(view_descriptor, tag_keys_);
}

void Count::RegisterOpenTelemetryMetric() {
  OpenTelemetryMetricRecorder::GetInstance().RegisterCounterMetric(name_, description_);
}

void Count::RegisterView() {
  opencensus::stats::ViewDescriptor view_descriptor =
      opencensus::stats::ViewDescriptor()
          .set_name(name_)
          .set_description(description_)
          .set_measure(name_)
          .set_aggregation(opencensus::stats::Aggregation::Count());

  internal::RegisterAsView(view_descriptor, tag_keys_);
}

void Sum::RegisterOpenTelemetryMetric() {
  OpenTelemetryMetricRecorder::GetInstance().RegisterSumMetric(name_, description_);
}

void Sum::RegisterView() {
  opencensus::stats::ViewDescriptor view_descriptor =
      opencensus::stats::ViewDescriptor()
          .set_name(name_)
          .set_description(description_)
          .set_measure(name_)
          .set_aggregation(opencensus::stats::Aggregation::Sum());

  internal::RegisterAsView(view_descriptor, tag_keys_);
}

}  // namespace stats
}  // namespace ray
