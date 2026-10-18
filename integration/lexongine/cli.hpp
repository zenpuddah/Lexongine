#pragma once
#include "cli_options.hpp"
#include "file_result_store.hpp"
#include <ostream>
namespace lexongine::cli {
// The optional fault callback is a test/host injection seam, never a CLI flag.
int dispatch(std::span<const std::string_view>,std::string_view source_build,std::ostream&,std::ostream&,
             std::function<Result<void>(StoreFaultPoint)> fault={}) noexcept;
}
