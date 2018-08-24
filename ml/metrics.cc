// Copyright 2018 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ml/metrics.h"

#include <algorithm>
#include <string>
#include <vector>

#include <base/bind.h>
#include <base/files/file_path.h>
#include <base/sys_info.h>
#include <base/time/time.h>

namespace ml {

namespace {

// UMA metric names:
constexpr char kCpuUsageMetricName[] =
    "MachineLearningService.CpuUsageMilliPercent";
constexpr char kMojoConnectionEventMetricName[] =
    "MachineLearningService.MojoConnectionEvent";
constexpr char kPrivateMemoryMetricName[] =
    "MachineLearningService.PrivateMemoryKb";
constexpr char kPeakPrivateMemoryMetricName[] =
    "MachineLearningService.PeakPrivateMemoryKb";

// UMA histogram ranges:
constexpr int kCpuUsageMinMilliPercent = 1;       // 0.001%
constexpr int kCpuUsageMaxMilliPercent = 100000;  // 100%
constexpr int kCpuUsageBuckets = 25;
constexpr int kMemoryUsageMinKb = 10;         // 10 KB
constexpr int kMemoryUsageMaxKb = 100000000;  // 100 GB
constexpr int kMemoryUsageBuckets = 100;

// chromeos_metrics::CumulativeMetrics constants:
constexpr char kCumulativeMetricsBackingDir[] = "/var/lib/ml_service/metrics";
constexpr char kPeakPrivateMemoryCumulativeStatName[] =
    "peak_private_memory_kb";

constexpr base::TimeDelta kCumulativeMetricsUpdatePeriod =
    base::TimeDelta::FromMinutes(5);
constexpr base::TimeDelta kCumulativeMetricsReportPeriod =
    base::TimeDelta::FromDays(1);

void RecordCumulativeMetrics(
    MetricsLibrary* const metrics_library,
    chromeos_metrics::CumulativeMetrics* const cumulative_metrics) {
  metrics_library->SendToUMA(
      kPeakPrivateMemoryMetricName,
      cumulative_metrics->Get(kPeakPrivateMemoryCumulativeStatName),
      kMemoryUsageMinKb, kMemoryUsageMaxKb, kMemoryUsageBuckets);
}

}  // namespace

Metrics::Metrics()
    : process_metrics_(base::ProcessMetrics::CreateCurrentProcessMetrics()) {
  metrics_library_.Init();
}

void Metrics::StartCollectingProcessMetrics() {
  if (cumulative_metrics_) {
    LOG(WARNING) << "Multiple calls to StartCollectingProcessMetrics";
    return;
  }

  // Baseline the CPU usage counter in |process_metrics_| to be zero as of now.
  process_metrics_->GetCPUUsage();

  cumulative_metrics_ = std::make_unique<chromeos_metrics::CumulativeMetrics>(
      base::FilePath(kCumulativeMetricsBackingDir),
      std::vector<std::string>{kPeakPrivateMemoryCumulativeStatName},
      kCumulativeMetricsUpdatePeriod,
      base::Bind(&Metrics::UpdateAndRecordMetrics, base::Unretained(this),
                 true /*record_current_metrics*/),
      kCumulativeMetricsReportPeriod,
      base::Bind(&RecordCumulativeMetrics,
                 base::Unretained(&metrics_library_)));
}

void Metrics::UpdateCumulativeMetricsNow() {
  if (!cumulative_metrics_) {
    return;
  }
  UpdateAndRecordMetrics(false /*record_current_metrics*/,
                         cumulative_metrics_.get());
}

void Metrics::UpdateAndRecordMetrics(
    const bool record_current_metrics,
    chromeos_metrics::CumulativeMetrics* const cumulative_metrics) {

  // Query memory usage.
  base::WorkingSetKBytes usage;
  process_metrics_->GetWorkingSetKBytes(&usage);

  // Update max memory stats.
  cumulative_metrics->Max(kPeakPrivateMemoryCumulativeStatName,
                          static_cast<int64_t>(usage.priv));

  if (record_current_metrics) {
    // Record CPU usage (units = milli-percent i.e. 0.001%):
    const int cpu_usage_milli_percent =
        static_cast<int>(1000. * process_metrics_->GetCPUUsage() /
                         base::SysInfo::NumberOfProcessors());
    metrics_library_.SendToUMA(kCpuUsageMetricName, cpu_usage_milli_percent,
                               kCpuUsageMinMilliPercent,
                               kCpuUsageMaxMilliPercent, kCpuUsageBuckets);
    // Record memory usage:
    metrics_library_.SendToUMA(kPrivateMemoryMetricName, usage.priv,
                               kMemoryUsageMinKb, kMemoryUsageMaxKb,
                               kMemoryUsageBuckets);
  }
}

void Metrics::RecordMojoConnectionEvent(const MojoConnectionEvent event) {
  metrics_library_.SendEnumToUMA(kMojoConnectionEventMetricName,
                                 static_cast<int>(event),
                                 static_cast<int>(MojoConnectionEvent::kMax));
}

}  // namespace ml
