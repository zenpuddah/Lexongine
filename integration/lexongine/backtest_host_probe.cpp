#include "backtest_host_probe.hpp"
#include "native_files.hpp"
#include "metadata_xml.hpp"
#include <fcntl.h>
#include <unistd.h>
namespace lexongine::host_probe {
Result<std::shared_ptr<Controller>> Controller::create(std::array<Trial, 2> trials,
                                                       std::shared_ptr<ResourceAccount> account,
                                                       std::uint64_t maximum) noexcept {
    return wire::protect([&]() -> Result<std::shared_ptr<Controller>> {
        if (!account || !maximum || maximum > 1024 || trials[0].id == trials[1].id ||
            trials[0].request == trials[1].request) {
            return diagnostic(FailureCode::InvalidConfig,
                              "invalid independent trial probe configuration");
        }
        auto charge =
            wire::checked(account->reserve(ResourceKind::Graph, 4096 + maximum * sizeof(Intent)));
        std::string config = "test.toy_trials.v1";
        for (const auto &trial : trials) {
            wire::checked(validate_text(trial.id, 64, false));
            wire::checked(validate_text(trial.request, 64, false));
            config += "|" + std::to_string(trial.id.size()) + ":" + trial.id + "|" +
                      std::to_string(trial.request.size()) + ":" + trial.request + "|" +
                      std::to_string(trial.enabled) + "|" + std::to_string(trial.threshold_raw);
        }
        auto owned = std::shared_ptr<Controller>(new Controller{
            std::move(charge), std::move(trials), maximum, wire::checked(files::digest(config))});
        owned->intents_.reserve(static_cast<std::size_t>(maximum));
        return owned;
    });
}
Result<void> Controller::consume(const StepReceipt &receipt,
                                 std::optional<std::uint32_t> fail_before) noexcept {
    return wire::protect([&]() -> Result<void> {
        if (receipt.available_at_ns) {
            decision_ = std::max(decision_, *receipt.available_at_ns);
        }
        if (receipt.status == StepStatus::Gap) {
            if (!receipt.gap || !receipt.observations.empty()) {
                return diagnostic(FailureCode::UpdateProtocol,
                                  "gap receipt fabricated an observation");
            }
            ++gaps_;
            return ok();
        }
        if (receipt.status != StepStatus::Processed) {
            return ok();
        }
        ++processed_;
        for (std::uint32_t i = 0; i < trials_.size(); ++i) {
            auto &trial = trials_[i];
            auto observation = std::find_if(
                receipt.observations.begin(), receipt.observations.end(),
                [&](const auto &o) { return o.delivered && o.context.request == trial.request; });
            if (observation == receipt.observations.end()) {
                continue;
            }
            if (fail_before && *fail_before == i) {
                return diagnostic(FailureCode::CallbackException,
                                  "injected participating toy-controller failure");
            }
            const auto value = wire::checked(
                observation->value.read<quant::data::Price>(observation->port, decision_));
            ++trial.decisions;
            if (trial.enabled && value.value.raw() >= trial.threshold_raw) {
                if (intents_.size() == maximum_) {
                    return diagnostic(FailureCode::EvidenceQuota,
                                      "toy intent ledger exceeded admitted count");
                }
                intents_.push_back({i, intents_.size() + 1, observation->context.key.sequence,
                                    value.value.raw(),
                                    observation->value.frame()->available_at_ns});
            }
        }
        return ok();
    });
}
RequiredEvidenceSpec evidence_spec(std::string path) {
    return {"trial_intents", "test.toy_intents_v1",
            std::move(path), "create_new",
            65536,           1024,
            "on_close",      "stop"};
}
Result<RequiredEvidenceSink> evidence(std::shared_ptr<Controller> controller,
                                      RequiredEvidenceSpec spec,
                                      std::shared_ptr<ResourceAccount> account,
                                      bool fail_close) noexcept {
    return wire::protect([&]() -> Result<RequiredEvidenceSink> {
        struct State {
            Reservation charge;
            std::shared_ptr<Controller> controller;
            std::shared_ptr<ResourceAccount> account;
            RequiredEvidenceSpec spec;
            int fd = -1;
            ~State() {
                if (fd >= 0) {
                    static_cast<void>(::close(fd));
                }
            }
        };
        auto state = std::shared_ptr<State>(
            new State{wire::checked(account->reserve(ResourceKind::Evidence, 131072)),
                      std::move(controller), std::move(account), spec});
        return RequiredEvidenceSink{
            {},
            std::move(spec),
            [state]() -> Result<void> {
                state->fd = ::open(state->spec.absolute_path.c_str(),
                                   O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
                if (state->fd < 0) {
                    return diagnostic(FailureCode::EvidenceIO, "toy ledger create-new failed");
                }
                return ok();
            },
            {},
            {},
            [state, fail_close](const RunReport &) -> Result<EvidenceReference> {
                return wire::protect([&]() -> Result<EvidenceReference> {
                    if (fail_close) {
                        return diagnostic(FailureCode::EvidenceIO,
                                          "injected required toy-ledger close failure");
                    }
                    std::string text = "test.toy_intents_v1 experiment=" +
                                       wire::hex(state->controller->experiment()) + "\n";
                    text.reserve(static_cast<std::size_t>(state->spec.max_bytes));
                    for (std::size_t i = 0; i < state->controller->trials().size(); ++i) {
                        const auto &trial = state->controller->trials()[i];
                        text +=
                            "trial_config=" + std::to_string(i) + " id_hex=" +
                            wire::hex(std::as_bytes(std::span{trial.id.data(), trial.id.size()})) +
                            " request_hex=" +
                            wire::hex(std::as_bytes(
                                std::span{trial.request.data(), trial.request.size()})) +
                            " enabled=" + std::to_string(trial.enabled) +
                            " threshold_raw=" + std::to_string(trial.threshold_raw) +
                            " decisions=" + std::to_string(trial.decisions) + "\n";
                    }
                    for (const auto &intent : state->controller->intents()) {
                        auto row = "trial=" + std::to_string(intent.trial) +
                                   " occurrence=" + std::to_string(intent.occurrence) +
                                   " step=" + std::to_string(intent.step) +
                                   " raw=" + std::to_string(intent.feature_raw) +
                                   " available=" + std::to_string(intent.available_at_ns) + "\n";
                        if (row.size() > state->spec.max_bytes - text.size()) {
                            return diagnostic(FailureCode::EvidenceQuota,
                                              "toy ledger byte ceiling exceeded");
                        }
                        text += row;
                    }
                    return files::finish_open(state->fd, state->spec.name,
                                              state->spec.absolute_path, text, state->account,
                                              state->spec.max_bytes, FailureCode::EvidenceIO);
                });
            }};
    });
}
} // namespace lexongine::host_probe
