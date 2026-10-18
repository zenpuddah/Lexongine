#pragma once

#include "lexongine/registry.hpp"
#include "quant/data/observations.hpp"

namespace lexongine {
template<> struct PublicationTraits<quant::data::Price> {
    using type = quant::data::Price;
    static constexpr auto storage = StorageKind::ScalarCopy;
};
template<> struct PublicationTraits<quant::data::Bar> {
    using type = Immutable<quant::data::Bar>;
    static constexpr auto storage = StorageKind::ImmutableOwned;
};
}
namespace quant::lexongine {
struct ComponentRegistration {
    ::lexongine::ImplementationIdentity implementation;
    ::lexongine::ModulePin pin;
    ::lexongine::MissingIntervalPolicy missing_intervals;
};
struct ComponentTypes {
    ::lexongine::RegisteredType<data::Bar> bar;
    ::lexongine::RegisteredType<data::Price> price;
};
[[nodiscard]] ::lexongine::TypeContract price_contract();
[[nodiscard]] ::lexongine::TypeContract bar_contract();
[[nodiscard]] ::lexongine::Result<ComponentTypes> register_components(::lexongine::Registry&, const ComponentRegistration&) noexcept;
} // namespace quant::lexongine
