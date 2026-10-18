#pragma once
#include "lexongine/execution.hpp"
#include "quant/lexongine/components.hpp"
namespace lexongine::host_probe {
// Integration-contract probe only: no orders, fills, account valuation or PnL.
struct Trial {std::string id,request;bool enabled;std::int64_t threshold_raw;std::uint64_t decisions=0;};
struct Intent {std::uint32_t trial;std::uint64_t occurrence,step;std::int64_t feature_raw,available_at_ns;friend bool operator==(const Intent&,const Intent&)=default;};
class Controller {
public:
    static Result<std::shared_ptr<Controller>> create(std::array<Trial,2>,std::shared_ptr<ResourceAccount>,std::uint64_t max_intents) noexcept;
    Result<void> consume(const StepReceipt&,std::optional<std::uint32_t> fail_before_trial={}) noexcept;
    [[nodiscard]] std::span<const Intent> intents() const noexcept{return intents_;}
    [[nodiscard]] const auto& trials() const noexcept{return trials_;}
    [[nodiscard]] Sha256 experiment() const noexcept{return experiment_;}
    [[nodiscard]] std::uint64_t gaps() const noexcept{return gaps_;}
    [[nodiscard]] std::uint64_t processed() const noexcept{return processed_;}
private:
    Controller(Reservation charge,std::array<Trial,2> trials,std::uint64_t maximum,Sha256 experiment):charge_(std::move(charge)),trials_(std::move(trials)),maximum_(maximum),experiment_(experiment){}
    Reservation charge_;std::array<Trial,2> trials_;std::uint64_t maximum_;Sha256 experiment_;std::vector<Intent> intents_;
    std::uint64_t gaps_=0,processed_=0;std::int64_t decision_=std::numeric_limits<std::int64_t>::min();
};
RequiredEvidenceSpec evidence_spec(std::string absolute_path);
Result<RequiredEvidenceSink> evidence(std::shared_ptr<Controller>,RequiredEvidenceSpec,std::shared_ptr<ResourceAccount>,bool fail_close=false) noexcept;
}
