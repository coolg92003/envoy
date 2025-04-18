#include <memory>

#include "envoy/config/core/v3/config_source.pb.h"
#include "envoy/service/discovery/v3/discovery.pb.h"

#include "source/common/common/assert.h"
#include "source/common/config/config_provider_impl.h"
#include "source/common/protobuf/utility.h"

#include "test/common/config/dummy_config.pb.h"
#include "test/mocks/server/instance.h"
#include "test/test_common/simulated_time_system.h"
#include "test/test_common/utility.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace Envoy {
namespace Config {
namespace {

using testing::InSequence;

class DummyConfigProviderManager;

class DummyConfig : public Envoy::Config::ConfigProvider::Config {
public:
  DummyConfig() = default;
  explicit DummyConfig(const test::common::config::DummyConfig& config_proto) {
    protos_.push_back(config_proto);
  }
  void addProto(const test::common::config::DummyConfig& config_proto) {
    protos_.push_back(config_proto);
  }

  uint32_t numProtos() const { return protos_.size(); }

private:
  std::vector<test::common::config::DummyConfig> protos_;
};

class StaticDummyConfigProvider : public ImmutableConfigProviderBase {
public:
  StaticDummyConfigProvider(const test::common::config::DummyConfig& config_proto,
                            Server::Configuration::ServerFactoryContext& factory_context,
                            DummyConfigProviderManager& config_provider_manager);

  ~StaticDummyConfigProvider() override = default;

  // Envoy::Config::ConfigProvider
  ConfigConstSharedPtr getConfig() const override { return config_; }

  // Envoy::Config::ConfigProvider
  ConfigProtoVector getConfigProtos() const override { return {&config_proto_}; }

private:
  ConfigConstSharedPtr config_;
  test::common::config::DummyConfig config_proto_;
};

class DummyConfigSubscription : public ConfigSubscriptionInstance,
                                Envoy::Config::SubscriptionCallbacks {
public:
  DummyConfigSubscription(const uint64_t manager_identifier,
                          Server::Configuration::ServerFactoryContext& factory_context,
                          DummyConfigProviderManager& config_provider_manager);
  ~DummyConfigSubscription() override = default;

  // Envoy::Config::ConfigSubscriptionCommonBase
  void start() override {}

  // Envoy::Config::ConfigSubscriptionInstance
  ConfigProvider::ConfigConstSharedPtr
  onConfigProtoUpdate(const Protobuf::Message& config_proto) override {
    return std::make_shared<DummyConfig>(
        static_cast<const test::common::config::DummyConfig&>(config_proto));
  }

  // Envoy::Config::SubscriptionCallbacks
  absl::Status onConfigUpdate(const std::vector<DecodedResourceRef>& resources,
                              const std::string& version_info) override {
    const auto& config =
        dynamic_cast<const test::common::config::DummyConfig&>(resources[0].get().resource());
    if (checkAndApplyConfigUpdate(config, "dummy_config", version_info)) {
      config_proto_ = config;
    }

    return ConfigSubscriptionCommonBase::onConfigUpdate();
  }
  // Envoy::Config::SubscriptionCallbacks
  absl::Status onConfigUpdate(const std::vector<DecodedResourceRef>&,
                              const Protobuf::RepeatedPtrField<std::string>&,
                              const std::string&) override {
    PANIC("not implemented");
  }

  void onConfigUpdateFailed(Envoy::Config::ConfigUpdateFailureReason,
                            const EnvoyException*) override {}

  const absl::optional<test::common::config::DummyConfig>& configProto() const {
    return config_proto_;
  }

private:
  absl::optional<test::common::config::DummyConfig> config_proto_;
};
using DummyConfigSubscriptionSharedPtr = std::shared_ptr<DummyConfigSubscription>;

class DummyDynamicConfigProvider : public MutableConfigProviderCommonBase {
public:
  explicit DummyDynamicConfigProvider(DummyConfigSubscriptionSharedPtr&& subscription)
      : MutableConfigProviderCommonBase(std::move(subscription), ApiType::Full),
        subscription_(static_cast<DummyConfigSubscription*>(
            MutableConfigProviderCommonBase::subscription_.get())) {}

  ~DummyDynamicConfigProvider() override = default;

  DummyConfigSubscription& subscription() { return *subscription_; }

  // Envoy::Config::ConfigProvider
  ConfigProtoVector getConfigProtos() const override {
    if (subscription_->configProto().has_value()) {
      return {&subscription_->configProto().value()};
    }
    return {};
  }

private:
  // Lifetime of this pointer is owned by the shared_ptr held by the base class.
  DummyConfigSubscription* subscription_;
};

class DummyConfigProviderManager : public ConfigProviderManagerImplBase {
public:
  explicit DummyConfigProviderManager(Server::Admin& admin)
      : ConfigProviderManagerImplBase(admin, "dummy") {}

  ~DummyConfigProviderManager() override = default;

  ProtobufTypes::MessagePtr dumpConfigs() const {
    return dumpConfigs(Matchers::UniversalStringMatcher());
  }

  // Envoy::Config::ConfigProviderManagerImplBase
  ProtobufTypes::MessagePtr dumpConfigs(const Matchers::StringMatcher&) const override {
    auto config_dump = std::make_unique<test::common::config::DummyConfigsDump>();
    for (const auto& element : configSubscriptions()) {
      auto subscription = element.second.lock();
      ASSERT(subscription);

      if (subscription->configInfo()) {
        auto* dynamic_config = config_dump->mutable_dynamic_dummy_configs()->Add();
        dynamic_config->set_version_info(subscription->configInfo().value().last_config_version_);
        dynamic_config->mutable_dummy_config()->MergeFrom(
            static_cast<DummyConfigSubscription*>(subscription.get())->configProto().value());
        TimestampUtil::systemClockToTimestamp(subscription->lastUpdated(),
                                              *dynamic_config->mutable_last_updated());
      }
    }

    for (const auto* provider : immutableConfigProviders(ConfigProviderInstanceType::Static)) {
      ASSERT(provider->configProtoInfoVector<test::common::config::DummyConfig>().has_value());
      auto* static_config = config_dump->mutable_static_dummy_configs()->Add();
      static_config->mutable_dummy_config()->MergeFrom(
          *provider->configProtoInfoVector<test::common::config::DummyConfig>()
               .value()
               .config_protos_[0]);
      TimestampUtil::systemClockToTimestamp(provider->lastUpdated(),
                                            *static_config->mutable_last_updated());
    }

    return config_dump;
  }

  // Envoy::Config::ConfigProviderManager
  ConfigProviderPtr
  createXdsConfigProvider(const Protobuf::Message& config_source_proto,
                          Server::Configuration::ServerFactoryContext& factory_context,
                          Init::Manager& init_manager, const std::string&,
                          const Envoy::Config::ConfigProviderManager::OptionalArg&) override {
    DummyConfigSubscriptionSharedPtr subscription = getSubscription<DummyConfigSubscription>(
        config_source_proto, init_manager,
        [&factory_context](const uint64_t manager_identifier,
                           ConfigProviderManagerImplBase& config_provider_manager)
            -> ConfigSubscriptionCommonBaseSharedPtr {
          return std::make_shared<DummyConfigSubscription>(
              manager_identifier, factory_context,
              static_cast<DummyConfigProviderManager&>(config_provider_manager));
        });

    return std::make_unique<DummyDynamicConfigProvider>(std::move(subscription));
  }

  // Envoy::Config::ConfigProviderManager
  ConfigProviderPtr
  createStaticConfigProvider(ProtobufTypes::ConstMessagePtrVector&& configs,
                             Server::Configuration::ServerFactoryContext& factory_context,
                             const OptionalArg&) override {
    // Currently, the dummy config provider only supports a single static config. We can extend this
    // to support multiple static configs if needed.
    ASSERT(configs.size() == 1);
    auto config = dynamic_cast<const test::common::config::DummyConfig&>(*configs[0]);
    return std::make_unique<StaticDummyConfigProvider>(config, factory_context, *this);
  }
};

DummyConfigSubscription::DummyConfigSubscription(
    const uint64_t manager_identifier, Server::Configuration::ServerFactoryContext& factory_context,
    DummyConfigProviderManager& config_provider_manager)
    : ConfigSubscriptionInstance("DummyDS", manager_identifier, config_provider_manager,
                                 factory_context) {
  // A nullptr is shared as the initial value.
  initialize(nullptr);
}

StaticDummyConfigProvider::StaticDummyConfigProvider(
    const test::common::config::DummyConfig& config_proto,
    Server::Configuration::ServerFactoryContext& factory_context,
    DummyConfigProviderManager& config_provider_manager)
    : ImmutableConfigProviderBase(factory_context, config_provider_manager,
                                  ConfigProviderInstanceType::Static, ApiType::Full),
      config_(std::make_shared<DummyConfig>(config_proto)), config_proto_(config_proto) {}

class ConfigProviderImplTest : public testing::Test {
public:
  void initialize() {
    EXPECT_CALL(server_factory_context_.admin_.config_tracker_, add_("dummy", _));
    provider_manager_ =
        std::make_unique<DummyConfigProviderManager>(server_factory_context_.admin_);
  }

  Event::SimulatedTimeSystem& timeSystem() { return time_system_; }

protected:
  Event::SimulatedTimeSystem time_system_;
  NiceMock<Server::Configuration::MockServerFactoryContext> server_factory_context_;
  NiceMock<Init::MockManager> init_manager_;
  std::unique_ptr<DummyConfigProviderManager> provider_manager_;
};

test::common::config::DummyConfig parseDummyConfigFromYaml(const std::string& yaml) {
  test::common::config::DummyConfig config;
  TestUtility::loadFromYaml(yaml, config);
  return config;
}

std::unique_ptr<test::common::config::DummyConfig>
parseDummyConfigPtrFromYaml(const std::string& yaml) {
  auto config = std::make_unique<test::common::config::DummyConfig>();
  TestUtility::loadFromYaml(yaml, *config);
  return config;
}

// Tests that dynamic config providers share ownership of the config
// subscriptions, config protos and data structures generated as a result of the
// configurations (i.e., the ConfigProvider::Config).
TEST_F(ConfigProviderImplTest, SharedOwnership) {
  initialize();
  Init::ExpectableWatcherImpl watcher;
  init_manager_.initialize(watcher);

  envoy::config::core::v3::ApiConfigSource config_source_proto;
  config_source_proto.set_api_type(envoy::config::core::v3::ApiConfigSource::GRPC);
  ConfigProviderPtr provider1 = provider_manager_->createXdsConfigProvider(
      config_source_proto, server_factory_context_, init_manager_, "dummy_prefix",
      ConfigProviderManager::NullOptionalArg());

  // No config protos have been received via the subscription yet.
  EXPECT_FALSE(provider1->configProtoInfoVector<test::common::config::DummyConfig>().has_value());

  const auto dummy_config = parseDummyConfigFromYaml("a: a dummy config");

  DummyConfigSubscription& subscription =
      dynamic_cast<DummyDynamicConfigProvider&>(*provider1).subscription();
  const auto decoded_resources = TestUtility::decodeResources({dummy_config}, "a");
  ASSERT_TRUE(subscription.onConfigUpdate(decoded_resources.refvec_, "1").ok());

  // Check that a newly created provider with the same config source will share
  // the subscription, config proto and resulting ConfigProvider::Config.
  ConfigProviderPtr provider2 = provider_manager_->createXdsConfigProvider(
      config_source_proto, server_factory_context_, init_manager_, "dummy_prefix",
      ConfigProviderManager::NullOptionalArg());

  EXPECT_TRUE(provider2->configProtoInfoVector<test::common::config::DummyConfig>().has_value());
  EXPECT_EQ(&dynamic_cast<DummyDynamicConfigProvider&>(*provider1).subscription(),
            &dynamic_cast<DummyDynamicConfigProvider&>(*provider2).subscription());
  EXPECT_EQ(provider1->config<const DummyConfig>().get(),
            provider2->config<const DummyConfig>().get());

  // Change the config source and verify that a new subscription is used.
  config_source_proto.set_api_type(envoy::config::core::v3::ApiConfigSource::REST);
  ConfigProviderPtr provider3 = provider_manager_->createXdsConfigProvider(
      config_source_proto, server_factory_context_, init_manager_, "dummy_prefix",
      ConfigProviderManager::NullOptionalArg());

  EXPECT_NE(&dynamic_cast<DummyDynamicConfigProvider&>(*provider1).subscription(),
            &dynamic_cast<DummyDynamicConfigProvider&>(*provider3).subscription());
  EXPECT_NE(provider1->config<const DummyConfig>().get(),
            provider3->config<const DummyConfig>().get());

  ASSERT_TRUE(dynamic_cast<DummyDynamicConfigProvider&>(*provider3)
                  .subscription()
                  .onConfigUpdate(decoded_resources.refvec_, "provider3")
                  .ok());

  EXPECT_EQ(2UL, static_cast<test::common::config::DummyConfigsDump*>(
                     provider_manager_->dumpConfigs().get())
                     ->dynamic_dummy_configs()
                     .size());

  // Test that tear down of config providers leads to correctly updating
  // centralized state; this is validated using the config dump.
  provider1.reset();
  provider2.reset();

  auto dynamic_dummy_configs =
      static_cast<test::common::config::DummyConfigsDump*>(provider_manager_->dumpConfigs().get())
          ->dynamic_dummy_configs();
  EXPECT_EQ(1UL, dynamic_dummy_configs.size());

  EXPECT_EQ("provider3", dynamic_dummy_configs[0].version_info());

  provider3.reset();

  EXPECT_EQ(0UL, static_cast<test::common::config::DummyConfigsDump*>(
                     provider_manager_->dumpConfigs().get())
                     ->dynamic_dummy_configs()
                     .size());
}

// A ConfigProviderManager that returns a dummy ConfigProvider.
class DummyConfigProviderManagerMockConfigProvider : public DummyConfigProviderManager {
public:
  DummyConfigProviderManagerMockConfigProvider(Server::Admin& admin)
      : DummyConfigProviderManager(admin) {}

  ConfigProviderPtr
  createXdsConfigProvider(const Protobuf::Message& config_source_proto,
                          Server::Configuration::ServerFactoryContext& factory_context,
                          Init::Manager& init_manager, const std::string&,
                          const Envoy::Config::ConfigProviderManager::OptionalArg&) override {
    DummyConfigSubscriptionSharedPtr subscription = getSubscription<DummyConfigSubscription>(
        config_source_proto, init_manager,
        [&factory_context](const uint64_t manager_identifier,
                           ConfigProviderManagerImplBase& config_provider_manager)
            -> ConfigSubscriptionCommonBaseSharedPtr {
          return std::make_shared<DummyConfigSubscription>(
              manager_identifier, factory_context,
              static_cast<DummyConfigProviderManagerMockConfigProvider&>(config_provider_manager));
        });
    return std::make_unique<DummyDynamicConfigProvider>(std::move(subscription));
  }
};

// Test that duplicate config updates will not trigger creation of a new ConfigProvider::Config.
TEST_F(ConfigProviderImplTest, DuplicateConfigProto) {
  InSequence sequence;
  // This provider manager returns a DummyDynamicConfigProvider.
  auto provider_manager = std::make_unique<DummyConfigProviderManagerMockConfigProvider>(
      server_factory_context_.admin_);
  envoy::config::core::v3::ApiConfigSource config_source_proto;
  config_source_proto.set_api_type(envoy::config::core::v3::ApiConfigSource::GRPC);
  ConfigProviderPtr provider = provider_manager->createXdsConfigProvider(
      config_source_proto, server_factory_context_, init_manager_, "dummy_prefix",
      ConfigProviderManager::NullOptionalArg());
  auto* typed_provider = static_cast<DummyDynamicConfigProvider*>(provider.get());
  auto& subscription = static_cast<DummyConfigSubscription&>(typed_provider->subscription());
  EXPECT_EQ(subscription.getConfig(), nullptr);
  // First time issuing a configUpdate(). A new ConfigProvider::Config should be created.
  const auto dummy_config = parseDummyConfigFromYaml("a: a dynamic dummy config");
  const auto decoded_resources = TestUtility::decodeResources({dummy_config}, "a");
  ASSERT_TRUE(subscription.onConfigUpdate(decoded_resources.refvec_, "1").ok());
  EXPECT_NE(subscription.getConfig(), nullptr);
  auto config_ptr = subscription.getConfig();
  EXPECT_EQ(typed_provider->config<DummyConfig>().get(), config_ptr.get());
  // Second time issuing the configUpdate(), this time with a duplicate proto. A new
  // ConfigProvider::Config _should not_ be created.
  ASSERT_TRUE(subscription.onConfigUpdate(decoded_resources.refvec_, "2").ok());
  EXPECT_EQ(config_ptr, subscription.getConfig());
  EXPECT_EQ(typed_provider->config<DummyConfig>().get(), config_ptr.get());
}

// An empty config provider tests on base class' constructor.
class InlineDummyConfigProvider : public ImmutableConfigProviderBase {
public:
  InlineDummyConfigProvider(Server::Configuration::ServerFactoryContext& factory_context,
                            DummyConfigProviderManager& config_provider_manager,
                            ConfigProviderInstanceType instance_type)
      : ImmutableConfigProviderBase(factory_context, config_provider_manager, instance_type,
                                    ApiType::Full) {}
  ConfigConstSharedPtr getConfig() const override { return nullptr; }
  ConfigProtoVector getConfigProtos() const override { return {}; }
};

class ConfigProviderImplDeathTest : public ConfigProviderImplTest {};

TEST_F(ConfigProviderImplDeathTest, AssertionFailureOnIncorrectInstanceType) {
  initialize();

  InlineDummyConfigProvider foo(server_factory_context_, *provider_manager_,
                                ConfigProviderInstanceType::Inline);
  InlineDummyConfigProvider bar(server_factory_context_, *provider_manager_,
                                ConfigProviderInstanceType::Static);
  EXPECT_DEBUG_DEATH(InlineDummyConfigProvider(server_factory_context_, *provider_manager_,
                                               ConfigProviderInstanceType::Xds),
                     "");
}

// Tests that the base ConfigProvider*s are handling registration with the
// /config_dump admin handler as well as generic bookkeeping such as timestamp
// updates.
TEST_F(ConfigProviderImplTest, ConfigDump) {
  initialize();
  // Empty dump first.
  auto message_ptr =
      server_factory_context_.admin_.config_tracker_.config_tracker_callbacks_["dummy"](
          Matchers::UniversalStringMatcher());
  const auto& dummy_config_dump =
      static_cast<const test::common::config::DummyConfigsDump&>(*message_ptr);

  test::common::config::DummyConfigsDump expected_config_dump;
  TestUtility::loadFromYaml(R"EOF(
static_dummy_configs:
dynamic_dummy_configs:
)EOF",
                            expected_config_dump);
  EXPECT_EQ(expected_config_dump.DebugString(), dummy_config_dump.DebugString());

  // Static config dump only.
  timeSystem().setSystemTime(std::chrono::milliseconds(1234567891234));

  ProtobufTypes::ConstMessagePtrVector configs;
  configs.push_back(parseDummyConfigPtrFromYaml("a: a static dummy config"));
  ConfigProviderPtr static_config = provider_manager_->createStaticConfigProvider(
      std::move(configs), server_factory_context_, ConfigProviderManager::NullOptionalArg());
  message_ptr = server_factory_context_.admin_.config_tracker_.config_tracker_callbacks_["dummy"](
      Matchers::UniversalStringMatcher());
  const auto& dummy_config_dump2 =
      static_cast<const test::common::config::DummyConfigsDump&>(*message_ptr);
  TestUtility::loadFromYaml(R"EOF(
static_dummy_configs:
  - dummy_config: { a: a static dummy config }
    last_updated: { seconds: 1234567891, nanos: 234000000 }
dynamic_dummy_configs:
)EOF",
                            expected_config_dump);
  EXPECT_EQ(expected_config_dump.DebugString(), dummy_config_dump2.DebugString());

  envoy::config::core::v3::ApiConfigSource config_source_proto;
  config_source_proto.set_api_type(envoy::config::core::v3::ApiConfigSource::GRPC);
  ConfigProviderPtr dynamic_provider = provider_manager_->createXdsConfigProvider(
      config_source_proto, server_factory_context_, init_manager_, "dummy_prefix",
      ConfigProviderManager::NullOptionalArg());

  // Static + dynamic config dump.
  const auto dummy_config = parseDummyConfigFromYaml("a: a dynamic dummy config");

  timeSystem().setSystemTime(std::chrono::milliseconds(1234567891567));
  DummyConfigSubscription& subscription =
      dynamic_cast<DummyDynamicConfigProvider&>(*dynamic_provider).subscription();
  const auto decoded_resources = TestUtility::decodeResources({dummy_config}, "a");
  ASSERT_TRUE(subscription.onConfigUpdate(decoded_resources.refvec_, "v1").ok());

  message_ptr = server_factory_context_.admin_.config_tracker_.config_tracker_callbacks_["dummy"](
      Matchers::UniversalStringMatcher());
  const auto& dummy_config_dump3 =
      static_cast<const test::common::config::DummyConfigsDump&>(*message_ptr);
  TestUtility::loadFromYaml(R"EOF(
static_dummy_configs:
  - dummy_config: { a: a static dummy config }
    last_updated: { seconds: 1234567891, nanos: 234000000 }
dynamic_dummy_configs:
  - version_info: v1
    dummy_config: { a: a dynamic dummy config }
    last_updated: { seconds: 1234567891, nanos: 567000000 }
)EOF",
                            expected_config_dump);
  EXPECT_EQ(expected_config_dump.DebugString(), dummy_config_dump3.DebugString());

  ProtobufTypes::ConstMessagePtrVector configs2;
  configs2.push_back(parseDummyConfigPtrFromYaml("a: another static dummy config"));
  ConfigProviderPtr static_config2 = provider_manager_->createStaticConfigProvider(
      std::move(configs2), server_factory_context_, ConfigProviderManager::NullOptionalArg());
  message_ptr = server_factory_context_.admin_.config_tracker_.config_tracker_callbacks_["dummy"](
      Matchers::UniversalStringMatcher());
  const auto& dummy_config_dump4 =
      static_cast<const test::common::config::DummyConfigsDump&>(*message_ptr);
  TestUtility::loadFromYaml(R"EOF(
static_dummy_configs:
  - dummy_config: { a: another static dummy config }
    last_updated: { seconds: 1234567891, nanos: 567000000 }
  - dummy_config: { a: a static dummy config }
    last_updated: { seconds: 1234567891, nanos: 234000000 }
dynamic_dummy_configs:
  - version_info: v1
    dummy_config: { a: a dynamic dummy config }
    last_updated: { seconds: 1234567891, nanos: 567000000 }
)EOF",
                            expected_config_dump);
  EXPECT_THAT(expected_config_dump, ProtoEqIgnoreRepeatedFieldOrdering(dummy_config_dump4));
}

// Tests that dynamic config providers enforce that the context's localInfo is
// set, since it is used to obtain the node/cluster attributes required for
// subscriptions.
TEST_F(ConfigProviderImplTest, LocalInfoNotDefined) {
  initialize();
  server_factory_context_.local_info_.node_.clear_cluster();
  server_factory_context_.local_info_.node_.clear_id();

  envoy::config::core::v3::ApiConfigSource config_source_proto;
  config_source_proto.set_api_type(envoy::config::core::v3::ApiConfigSource::GRPC);
  EXPECT_THROW_WITH_MESSAGE(
      provider_manager_->createXdsConfigProvider(config_source_proto, server_factory_context_,
                                                 init_manager_, "dummy_prefix",
                                                 ConfigProviderManager::NullOptionalArg()),
      EnvoyException,
      "DummyDS: node 'id' and 'cluster' are required. Set it either in 'node' config or "
      "via --service-node and --service-cluster options.");
}

class DeltaDummyConfigProviderManager;

class DeltaDummyConfigSubscription : public DeltaConfigSubscriptionInstance,
                                     Envoy::Config::SubscriptionCallbacks {
public:
  using ProtoMap = std::map<std::string, test::common::config::DummyConfig>;

  DeltaDummyConfigSubscription(const uint64_t manager_identifier,
                               Server::Configuration::ServerFactoryContext& factory_context,
                               DeltaDummyConfigProviderManager& config_provider_manager);

  // Envoy::Config::ConfigSubscriptionCommonBase
  void start() override {}

  // Envoy::Config::SubscriptionCallbacks
  absl::Status onConfigUpdate(const std::vector<DecodedResourceRef>& resources,
                              const std::string& version_info) override {
    if (resources.empty()) {
      return absl::OkStatus();
    }

    // For simplicity, there is no logic here to track updates and/or removals to the existing
    // config proto set (i.e., this is append only). Real xDS APIs will need to track additions,
    // updates and removals to the config set and apply the diffs to the underlying config
    // implementations.
    for (const auto& resource : resources) {
      const auto& dummy_config =
          dynamic_cast<const test::common::config::DummyConfig&>(resource.get().resource());
      proto_map_[version_info] = dummy_config;
      // Propagate the new config proto to all worker threads.
      applyConfigUpdate([&dummy_config](ConfigProvider::ConfigConstSharedPtr prev_config)
                            -> ConfigProvider::ConfigConstSharedPtr {
        auto* config = const_cast<DummyConfig*>(static_cast<const DummyConfig*>(prev_config.get()));
        // Per above, append only for now.
        config->addProto(dummy_config);
        return prev_config;
      });
    }

    absl::Status status = ConfigSubscriptionCommonBase::onConfigUpdate();
    if (!status.ok()) {
      return status;
    }
    setLastConfigInfo(absl::optional<LastConfigInfo>({absl::nullopt, version_info}));
    return absl::OkStatus();
  }
  absl::Status onConfigUpdate(const std::vector<DecodedResourceRef>&,
                              const Protobuf::RepeatedPtrField<std::string>&,
                              const std::string&) override {
    PANIC("not implemented");
  }
  void onConfigUpdateFailed(Envoy::Config::ConfigUpdateFailureReason,
                            const EnvoyException*) override {
    ConfigSubscriptionCommonBase::onConfigUpdateFailed();
  }
  const ProtoMap& protoMap() const { return proto_map_; }

private:
  ProtoMap proto_map_;
};
using DeltaDummyConfigSubscriptionSharedPtr = std::shared_ptr<DeltaDummyConfigSubscription>;

class DeltaDummyDynamicConfigProvider : public Envoy::Config::MutableConfigProviderCommonBase {
public:
  DeltaDummyDynamicConfigProvider(DeltaDummyConfigSubscriptionSharedPtr&& subscription)
      : MutableConfigProviderCommonBase(std::move(subscription), ConfigProvider::ApiType::Delta),
        subscription_(static_cast<DeltaDummyConfigSubscription*>(
            MutableConfigProviderCommonBase::subscription_.get())) {}

  DeltaDummyConfigSubscription& subscription() { return *subscription_; }

  // Envoy::Config::ConfigProvider
  ConfigProtoVector getConfigProtos() const override {
    ConfigProtoVector proto_vector;
    for (const auto& value_type : subscription_->protoMap()) {
      proto_vector.push_back(&value_type.second);
    }
    return proto_vector;
  }

private:
  DeltaDummyConfigSubscription* subscription_;
};

class DeltaDummyConfigProviderManager : public ConfigProviderManagerImplBase {
public:
  DeltaDummyConfigProviderManager(Server::Admin& admin)
      : ConfigProviderManagerImplBase(admin, "dummy") {}

  ProtobufTypes::MessagePtr dumpConfigs() const {
    return dumpConfigs(Matchers::UniversalStringMatcher());
  }

  // Envoy::Config::ConfigProviderManagerImplBase
  ProtobufTypes::MessagePtr dumpConfigs(const Matchers::StringMatcher&) const override {
    auto config_dump = std::make_unique<test::common::config::DeltaDummyConfigsDump>();
    for (const auto& element : configSubscriptions()) {
      auto subscription = element.second.lock();
      ASSERT(subscription);

      if (subscription->configInfo()) {
        auto* dynamic_config = config_dump->mutable_dynamic_dummy_configs()->Add();
        dynamic_config->set_version_info(subscription->configInfo().value().last_config_version_);
        const auto* typed_subscription =
            static_cast<DeltaDummyConfigSubscription*>(subscription.get());
        const DeltaDummyConfigSubscription::ProtoMap& proto_map = typed_subscription->protoMap();
        for (const auto& value_type : proto_map) {
          dynamic_config->mutable_dummy_configs()->Add()->MergeFrom(value_type.second);
        }
        TimestampUtil::systemClockToTimestamp(subscription->lastUpdated(),
                                              *dynamic_config->mutable_last_updated());
      }
    }

    return config_dump;
  }

  // Envoy::Config::ConfigProviderManager
  ConfigProviderPtr
  createXdsConfigProvider(const Protobuf::Message& config_source_proto,
                          Server::Configuration::ServerFactoryContext& factory_context,
                          Init::Manager& init_manager, const std::string&,
                          const Envoy::Config::ConfigProviderManager::OptionalArg&) override {
    DeltaDummyConfigSubscriptionSharedPtr subscription =

        getSubscription<DeltaDummyConfigSubscription>(
            config_source_proto, init_manager,
            [&factory_context](const uint64_t manager_identifier,
                               ConfigProviderManagerImplBase& config_provider_manager)
                -> ConfigSubscriptionCommonBaseSharedPtr {
              return std::make_shared<DeltaDummyConfigSubscription>(
                  manager_identifier, factory_context,
                  static_cast<DeltaDummyConfigProviderManager&>(config_provider_manager));
            });

    return std::make_unique<DeltaDummyDynamicConfigProvider>(std::move(subscription));
  }

  ConfigProviderPtr createStaticConfigProvider(ProtobufTypes::ConstMessagePtrVector&&,
                                               Server::Configuration::ServerFactoryContext&,
                                               const OptionalArg&) override {
    return nullptr;
  };
};

DeltaDummyConfigSubscription::DeltaDummyConfigSubscription(
    const uint64_t manager_identifier, Server::Configuration::ServerFactoryContext& factory_context,
    DeltaDummyConfigProviderManager& config_provider_manager)
    : DeltaConfigSubscriptionInstance("Dummy", manager_identifier, config_provider_manager,
                                      factory_context) {
  initialize(
      []() -> ConfigProvider::ConfigConstSharedPtr { return std::make_shared<DummyConfig>(); });
}

class DeltaConfigProviderImplTest : public testing::Test {
public:
  DeltaConfigProviderImplTest() {
    EXPECT_CALL(server_factory_context_.admin_.config_tracker_, add_("dummy", _));
    provider_manager_ =
        std::make_unique<DeltaDummyConfigProviderManager>(server_factory_context_.admin_);
  }

  Event::SimulatedTimeSystem& timeSystem() { return time_system_; }

protected:
  Event::SimulatedTimeSystem time_system_;
  NiceMock<Server::Configuration::MockServerFactoryContext> server_factory_context_;
  NiceMock<Init::MockManager> init_manager_;
  std::unique_ptr<DeltaDummyConfigProviderManager> provider_manager_;
};

// Validate that delta config subscriptions are shared across delta dynamic config providers and
// that the underlying Config implementation can be shared as well.
TEST_F(DeltaConfigProviderImplTest, MultipleDeltaSubscriptions) {
  envoy::config::core::v3::ApiConfigSource config_source_proto;
  config_source_proto.set_api_type(envoy::config::core::v3::ApiConfigSource::GRPC);
  ConfigProviderPtr provider1 = provider_manager_->createXdsConfigProvider(
      config_source_proto, server_factory_context_, init_manager_, "dummy_prefix",
      ConfigProviderManager::NullOptionalArg());

  // No config protos have been received via the subscription yet.
  EXPECT_FALSE(provider1->configProtoInfoVector<test::common::config::DummyConfig>().has_value());

  const auto dummy_config_0 = parseDummyConfigFromYaml("a: a dummy config");
  const auto dummy_config_1 = parseDummyConfigFromYaml("a: another dummy config");
  const auto decoded_resources =
      TestUtility::decodeResources({dummy_config_0, dummy_config_1}, "a");

  DeltaDummyConfigSubscription& subscription =
      dynamic_cast<DeltaDummyDynamicConfigProvider&>(*provider1).subscription();
  ASSERT_TRUE(subscription.onConfigUpdate(decoded_resources.refvec_, "1").ok());

  ConfigProviderPtr provider2 = provider_manager_->createXdsConfigProvider(
      config_source_proto, server_factory_context_, init_manager_, "dummy_prefix",
      ConfigProviderManager::NullOptionalArg());

  // Providers, config implementations (i.e., the DummyConfig) and config protos are
  // expected to be shared for a given subscription.
  EXPECT_EQ(&dynamic_cast<DeltaDummyDynamicConfigProvider&>(*provider1).subscription(),
            &dynamic_cast<DeltaDummyDynamicConfigProvider&>(*provider2).subscription());
  ASSERT_TRUE(provider2->configProtoInfoVector<test::common::config::DummyConfig>().has_value());
  EXPECT_EQ(
      provider1->configProtoInfoVector<test::common::config::DummyConfig>().value().config_protos_,
      provider2->configProtoInfoVector<test::common::config::DummyConfig>().value().config_protos_);
  EXPECT_EQ(provider1->config<const DummyConfig>().get(),
            provider2->config<const DummyConfig>().get());
  // Validate that the config protos are propagated to the thread local config implementation.
  EXPECT_EQ(provider1->config<const DummyConfig>()->numProtos(), 2);

  // Issue a second config update to validate that having multiple providers bound to the
  // subscription causes a single update to the underlying shared config implementation.
  ASSERT_TRUE(subscription.onConfigUpdate(decoded_resources.refvec_, "2").ok());
  // NOTE: the config implementation is append only and _does not_ track updates/removals to the
  // config proto set, so the expectation is to double the size of the set.
  EXPECT_EQ(provider1->config<const DummyConfig>().get(),
            provider2->config<const DummyConfig>().get());
  EXPECT_EQ(provider1->config<const DummyConfig>()->numProtos(), 4);
}

// Tests a config update failure.
TEST_F(DeltaConfigProviderImplTest, DeltaSubscriptionFailure) {
  envoy::config::core::v3::ApiConfigSource config_source_proto;
  config_source_proto.set_api_type(envoy::config::core::v3::ApiConfigSource::GRPC);
  ConfigProviderPtr provider = provider_manager_->createXdsConfigProvider(
      config_source_proto, server_factory_context_, init_manager_, "dummy_prefix",
      ConfigProviderManager::NullOptionalArg());
  DeltaDummyConfigSubscription& subscription =
      dynamic_cast<DeltaDummyDynamicConfigProvider&>(*provider).subscription();
  const auto time = std::chrono::milliseconds(1234567891234);
  timeSystem().setSystemTime(time);
  const EnvoyException ex(fmt::format("config failure"));
  // Verify the failure updates the lastUpdated() timestamp.
  subscription.onConfigUpdateFailed(Envoy::Config::ConfigUpdateFailureReason::ConnectionFailure,
                                    &ex);
  EXPECT_EQ(std::chrono::time_point_cast<std::chrono::milliseconds>(provider->lastUpdated())
                .time_since_epoch(),
            time);
}

} // namespace
} // namespace Config
} // namespace Envoy
