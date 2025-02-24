#include "source/common/common/utility.h"
#include "source/common/network/utility.h"
#include "source/common/router/upstream_request.h"

#include "test/common/http/common.h"
#include "test/mocks/router/router_filter_interface.h"
#include "test/test_common/test_runtime.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::_;
using testing::HasSubstr;
using testing::NiceMock;
using testing::Return;

namespace Envoy {
namespace Router {
namespace {

class UpstreamRequestTest : public testing::Test {
public:
  UpstreamRequestTest() {
    HttpTestUtility::addDefaultHeaders(downstream_request_header_map_);
    ON_CALL(router_filter_interface_, downstreamHeaders())
        .WillByDefault(Return(&downstream_request_header_map_));
  }

  void initialize() {
    auto conn_pool = std::make_unique<NiceMock<Router::MockGenericConnPool>>();
    conn_pool_ = conn_pool.get();
    upstream_request_ = std::make_unique<UpstreamRequest>(router_filter_interface_,
                                                          std::move(conn_pool), false, true);
  }
  Http::FilterFactoryCb createDecoderFilterFactoryCb(Http::StreamDecoderFilterSharedPtr filter) {
    return [filter](Http::FilterChainFactoryCallbacks& callbacks) {
      callbacks.addStreamDecoderFilter(filter);
    };
  }

  Router::MockGenericConnPool* conn_pool_{}; // Owned by the upstream request
  Http::TestRequestHeaderMapImpl downstream_request_header_map_{};
  NiceMock<MockRouterFilterInterface> router_filter_interface_;
  std::unique_ptr<UpstreamRequest> upstream_request_;
};

// UpstreamRequest is responsible processing for passing 101 upgrade headers to onUpstreamHeaders.
TEST_F(UpstreamRequestTest, Decode101UpgradeHeaders) {
  initialize();

  auto upgrade_headers = std::make_unique<Http::TestResponseHeaderMapImpl>(
      Http::TestResponseHeaderMapImpl({{":status", "101"}}));
  EXPECT_CALL(router_filter_interface_, onUpstreamHeaders(_, _, _, _));
  upstream_request_->decodeHeaders(std::move(upgrade_headers), false);
}

// UpstreamRequest is responsible for ignoring non-{100,101} 1xx headers.
TEST_F(UpstreamRequestTest, IgnoreOther1xxHeaders) {
  initialize();
  auto other_headers = std::make_unique<Http::TestResponseHeaderMapImpl>(
      Http::TestResponseHeaderMapImpl({{":status", "102"}}));
  EXPECT_CALL(router_filter_interface_, onUpstreamHeaders(_, _, _, _)).Times(0);
  upstream_request_->decodeHeaders(std::move(other_headers), false);
}

TEST_F(UpstreamRequestTest, TestAccessors) {
  initialize();
  auto response_headers = std::make_unique<Http::TestResponseHeaderMapImpl>(
      Http::TestResponseHeaderMapImpl({{":status", "200"}}));
  EXPECT_CALL(router_filter_interface_, onUpstreamHeaders(_, _, _, _));
  upstream_request_->decodeHeaders(std::move(response_headers), false);
}

// Test sending headers from the router to upstream.
TEST_F(UpstreamRequestTest, AcceptRouterHeaders) {
  TestScopedRuntime scoped_runtime;
  scoped_runtime.mergeValues({{"envoy.reloadable_features.allow_upstream_filters", "true"}});
  std::shared_ptr<Http::MockStreamDecoderFilter> filter(
      new NiceMock<Http::MockStreamDecoderFilter>());

  EXPECT_CALL(*router_filter_interface_.cluster_info_, createFilterChain)
      .WillOnce(Invoke([&](Http::FilterChainManager& manager) -> void {
        auto factory = createDecoderFilterFactoryCb(filter);
        manager.applyFilterFactoryCb({}, factory);
      }));

  initialize();
  ASSERT_TRUE(filter->callbacks_ != nullptr);

  // Test filter manager accessors.
  EXPECT_FALSE(filter->callbacks_->informationalHeaders().has_value());
  EXPECT_FALSE(filter->callbacks_->responseHeaders().has_value());
  EXPECT_FALSE(filter->callbacks_->http1StreamEncoderOptions().has_value());
  EXPECT_EQ(&filter->callbacks_->tracingConfig(),
            &router_filter_interface_.callbacks_.tracingConfig());
  EXPECT_EQ(filter->callbacks_->clusterInfo(), router_filter_interface_.callbacks_.clusterInfo());
  EXPECT_EQ(&filter->callbacks_->activeSpan(), &router_filter_interface_.callbacks_.activeSpan());
  EXPECT_EQ(&filter->callbacks_->streamInfo(), &router_filter_interface_.callbacks_.streamInfo());

  EXPECT_ENVOY_BUG(filter->callbacks_->requestRouteConfigUpdate(nullptr),
                   "requestRouteConfigUpdate called from upstream filter");
  EXPECT_ENVOY_BUG(filter->callbacks_->setRoute(nullptr),
                   "set route cache called from upstream filter");
  EXPECT_ENVOY_BUG(filter->callbacks_->clearRouteCache(),
                   "clear route cache called from upstream filter");

  EXPECT_CALL(*conn_pool_, newStream(_))
      .WillOnce(Invoke([&](GenericConnectionPoolCallbacks* callbacks) {
        std::stringstream out;
        callbacks->upstreamToDownstream().dumpState(out, 0);
        std::string state = out.str();
        EXPECT_THAT(state, testing::HasSubstr("UpstreamRequest"));
        EXPECT_EQ(callbacks->upstreamToDownstream().connection().ptr(),
                  router_filter_interface_.callbacks_.connection().ptr());
        return nullptr;
      }));

  EXPECT_CALL(*filter, decodeHeaders(_, false));
  upstream_request_->acceptHeadersFromRouter(false);

  EXPECT_CALL(router_filter_interface_.callbacks_, resetStream(_, _));
  filter->callbacks_->resetStream();
}

// UpstreamRequest dumpState without allocating memory.
TEST_F(UpstreamRequestTest, DumpsStateWithoutAllocatingMemory) {
  initialize();
  // Set up router filter
  auto connection_info_provider =
      router_filter_interface_.client_connection_.stream_info_.downstream_connection_info_provider_;
  connection_info_provider->setRemoteAddress(
      Network::Utility::parseInternetAddressAndPort("1.2.3.4:5678"));
  connection_info_provider->setLocalAddress(
      Network::Utility::parseInternetAddressAndPort("5.6.7.8:5678"));
  connection_info_provider->setDirectRemoteAddressForTest(
      Network::Utility::parseInternetAddressAndPort("1.2.3.4:5678"));

  // Dump State
  std::array<char, 1024> buffer;
  OutputBufferStream ostream{buffer.data(), buffer.size()};
  Stats::TestUtil::MemoryTest memory_test;
  upstream_request_->dumpState(ostream, 0);
  EXPECT_EQ(memory_test.consumedBytes(), 0);

  // Check Contents
  EXPECT_THAT(ostream.contents(), HasSubstr("UpstreamRequest "));
  EXPECT_THAT(ostream.contents(), HasSubstr("addressProvider: \n  ConnectionInfoSetterImpl "));
  EXPECT_THAT(ostream.contents(), HasSubstr("request_headers: \n"));
}

} // namespace
} // namespace Router
} // namespace Envoy
