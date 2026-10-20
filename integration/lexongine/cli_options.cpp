#include "cli_options.hpp"

namespace lexongine::cli {
Result<Options> parse_options(std::span<const std::string_view> args,
                              std::shared_ptr<ResourceAccount> account) noexcept {
    return guarded([&]() -> Result<Options> {
        if (!account || args.empty() || args.size() > max_arguments) {
            return diagnostic(FailureCode::InvalidConfig,
                              "command arguments exceed the CLI v1 contract");
        }
        for (const auto arg : args) {
            auto valid = validate_text(arg, max_argument_bytes, false);
            if (const auto *error = std::get_if<Diagnostic>(&valid)) {
                return *error;
            }
        }
        auto reserved = account->reserve(ResourceKind::Graph,
                                         4096 + args.size() * (2 * max_argument_bytes + 256));
        if (const auto *error = std::get_if<Diagnostic>(&reserved)) {
            return *error;
        }
        Options result;
        result.charge = std::get<Reservation>(std::move(reserved));
        if (args[0] == "list") {
            result.command = Command::List;
        } else if (args[0] == "describe") {
            result.command = Command::Describe;
        } else if (args[0] == "validate") {
            result.command = Command::Validate;
        } else if (args[0] == "plan") {
            result.command = Command::Plan;
        } else if (args[0] == "run") {
            result.command = Command::Run;
        } else {
            return diagnostic(FailureCode::InvalidConfig, "unknown CLI command");
        }
        const bool composition = result.command == Command::Validate ||
                                 result.command == Command::Plan || result.command == Command::Run;
        std::vector<std::string_view> seen;
        seen.reserve(args.size());
        for (std::size_t i = 1; i < args.size(); i += 2) {
            const auto option = args[i];
            if (i + 1 == args.size() || args[i + 1].starts_with("--")) {
                return diagnostic(FailureCode::InvalidConfig,
                                  "CLI option requires an explicit value");
            }
            const auto value = args[i + 1];
            if (option != "--set" && option != "--observe") {
                if (std::find(seen.begin(), seen.end(), option) != seen.end()) {
                    return diagnostic(FailureCode::InvalidConfig, "duplicate CLI option");
                }
                seen.push_back(option);
            }
            if (composition && option == "--config") {
                result.config = value;
            } else if (result.command == Command::Describe && option == "--operation") {
                result.operation = value;
            } else if (result.command == Command::Describe && option == "--implementation") {
                result.implementation = value;
            } else if (result.command == Command::Describe && option == "--version") {
                auto parsed = parse_integer<std::uint32_t>(value);
                if (const auto *error = std::get_if<Diagnostic>(&parsed)) {
                    return *error;
                }
                result.version = std::get<std::uint32_t>(parsed);
            } else if (result.command == Command::Plan && option == "--plan") {
                result.plan = value;
            } else if (result.command == Command::Plan && option == "--before-dot") {
                result.before_dot = value;
            } else if (result.command == Command::Plan && option == "--after-dot") {
                result.after_dot = value;
            } else if (result.command == Command::Run && option == "--report") {
                result.report = value;
            } else if (composition && option == "--retention-log") {
                result.overrides.retention_log = value;
            } else if (composition && option == "--sharing" &&
                       (value == "exact" || value == "disabled")) {
                result.overrides.sharing = value;
            } else if (composition && option == "--reuse" &&
                       (value == "automatic_read_write" || value == "disabled")) {
                result.overrides.reuse = value;
            } else if (composition && option == "--observe") {
                if (std::find(result.overrides.observations.begin(),
                              result.overrides.observations.end(),
                              value) != result.overrides.observations.end()) {
                    return diagnostic(FailureCode::InvalidConfig, "duplicate selected observation");
                }
                result.overrides.observations.emplace_back(value);
            } else if (composition && option == "--set") {
                const auto equals = value.find('=');
                const auto dot = value.substr(0, equals).rfind('.');
                if (equals == std::string_view::npos || dot == std::string_view::npos || dot == 0 ||
                    dot + 1 == equals) {
                    return diagnostic(FailureCode::InvalidConfig,
                                      "parameter override must be producer.PARAM=VALUE");
                }
                ParameterOverride replacement{std::string{value.substr(0, dot)},
                                              std::string{value.substr(dot + 1, equals - dot - 1)},
                                              std::string{value.substr(equals + 1)}, i + 1};
                for (const auto &prior : result.overrides.parameters) {
                    if (prior.producer == replacement.producer &&
                        prior.parameter == replacement.parameter) {
                        return diagnostic(FailureCode::InvalidConfig,
                                          "duplicate parameter override target");
                    }
                }
                result.overrides.parameters.push_back(std::move(replacement));
            } else {
                return diagnostic(FailureCode::InvalidConfig,
                                  "unknown, invalid or inapplicable CLI option");
            }
        }
        if ((composition && result.config.empty()) ||
            (result.command == Command::Describe &&
             (result.operation.empty() || result.implementation.empty() || !result.version)) ||
            (result.command == Command::Plan &&
             (result.plan.empty() || result.before_dot.empty() || result.after_dot.empty())) ||
            (result.command == Command::Run && result.report.empty())) {
            return diagnostic(FailureCode::InvalidConfig,
                              "command is missing required explicit options");
        }
        for (const auto *path :
             {&result.plan, &result.before_dot, &result.after_dot, &result.report}) {
            if (!path->empty() && !path->starts_with('/')) {
                return diagnostic(FailureCode::InvalidConfig,
                                  "evidence output path must be absolute");
            }
        }
        if (result.overrides.retention_log && !result.overrides.retention_log->starts_with('/')) {
            return diagnostic(FailureCode::InvalidConfig, "retention output path must be absolute");
        }
        return result;
    });
}
} // namespace lexongine::cli
