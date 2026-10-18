#pragma once

#include "lexongine/registry.hpp"

namespace lexongine::plugin_test {
// Explicit public SDK type shared by host and module. There is no string/size cast fallback.
struct __attribute__((visibility("default"))) Payload { std::vector<std::int64_t> values; };
struct OtherPayload { std::vector<std::int64_t> values; };
inline TypeContract payload_contract() { return {"lexongine.test.payload", 1, {1, {}}}; }
inline TypeContract scalar_contract() { return {"lexongine.test.scalar", 1, {1, {}}}; }
inline OperationKey operation_key() { return {"test", "plugin_source", 1, "native-test-v1"}; }
inline OperationKey consumer_key() { return {"test", "plugin_consumer", 1, "native-test-v1"}; }
inline Contract protocol(std::string id) { return {std::move(id), 1, {1, {}}}; }
}
namespace lexongine {
template<> struct PublicationTraits<std::int64_t> {
    using type = std::int64_t;
    static constexpr StorageKind storage = StorageKind::ScalarCopy;
};
template<> struct PublicationTraits<plugin_test::Payload> {
    using type = Immutable<plugin_test::Payload>;
    static constexpr StorageKind storage = StorageKind::ImmutableOwned;
};
template<> struct PublicationTraits<plugin_test::OtherPayload> {
    using type = Immutable<plugin_test::OtherPayload>;
    static constexpr StorageKind storage = StorageKind::ImmutableOwned;
};
}
