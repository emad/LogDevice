/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#include <chrono>
#include <memory>
#include <thread>

#include <folly/hash/Checksum.h>
#include <folly/Random.h>
#include <gtest/gtest.h>

#include "logdevice/common/configuration/Configuration.h"
#include "logdevice/common/LibeventTimer.h"
#include "logdevice/common/ReaderImpl.h"
#include "logdevice/common/stats/Stats.h"
#include "logdevice/common/test/TestUtil.h"

#include "logdevice/include/Client.h"
#include "logdevice/lib/ClientImpl.h"
#include "logdevice/test/utils/IntegrationTestBase.h"
#include "logdevice/test/utils/IntegrationTestUtils.h"

using namespace facebook::logdevice;

class RecordCacheIntegrationTest : public IntegrationTestBase {};

// test there should be no record cache miss for log recovery if cluster is
// taking new writes for logs for the first time
TEST_F(RecordCacheIntegrationTest, RecordCacheHitForNewAppends) {
  const int NNODES = 2;
  const int NLOGS = 1;

  Configuration::Log log_config =
      IntegrationTestUtils::ClusterFactory::createDefaultLogConfig(2);
  log_config.maxWritesInFlight = 1024;
  log_config.replicationFactor = 2;
  log_config.extraCopies = 0;

  auto cluster = IntegrationTestUtils::ClusterFactory()
                     .setParam("--enable-record-cache", "true")
                     .setNumLogs(NLOGS)
                     .setLogConfig(log_config)
                     .useHashBasedSequencerAssignment()
                     .create(NNODES);

  std::shared_ptr<Client> client =
      cluster->createClient(std::chrono::seconds(2));

  auto do_write = [&] {
    for (logid_t::raw_type log = 1; log <= NLOGS; ++log) {
      for (int i = 0; i < 10; ++i) {
        lsn_t lsn = client->appendSync(logid_t(log), Payload("dummy", 5));
        EXPECT_NE(LSN_INVALID, lsn);
      }
    }
  };

  do_write();

  int rv = cluster->waitForMetaDataLogWrites(std::chrono::steady_clock::now() +
                                             std::chrono::seconds(5));
  cluster->waitForRecovery();

  ld_info("disabling N1, expect sequencer re-activations...");
  for (auto n : {1}) {
    cluster->updateNodeAttributes(n, configuration::StorageState::READ_ONLY, 0);
  }
  cluster->waitForConfigUpdate();
  rv = cluster->waitForMetaDataLogWrites(std::chrono::steady_clock::now() +
                                         std::chrono::seconds(5));
  EXPECT_EQ(0, rv);
  cluster->waitForRecovery();

  auto get_stats_sum = [&](const std::string& name) {
    int64_t result = 0;
    for (node_index_t n = 0; n < NNODES; ++n) {
      result += cluster->getNode(n).stats()[name];
    }
    return result;
  };

  ASSERT_GT(get_stats_sum("recovery_success"), 0);
  ASSERT_GT(get_stats_sum("record_cache_seal_hit_datalog"), 0);
  ASSERT_GT(get_stats_sum("record_cache_digest_hit_datalog"), 0);

  // there should be no misses
  ASSERT_EQ(0, get_stats_sum("record_cache_seal_miss_datalog"));
  ASSERT_EQ(0, get_stats_sum("record_cache_digest_miss_datalog"));
}
