#include "file_evidence.hpp"
#include "quant/storage/file_integrity.hpp"

#include <cerrno>
#include <filesystem>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace lexongine {
namespace {
struct EvidenceFailure { Diagnostic error; };
template<class T> T checked(std::variant<T, Diagnostic> result) {
    if (const auto* error = std::get_if<Diagnostic>(&result)) { throw EvidenceFailure{*error}; } return std::get<T>(std::move(result));
}
void checked(Result<void> result) { if (const auto* error = std::get_if<Diagnostic>(&result)) { throw EvidenceFailure{*error}; } }
[[noreturn]] void fail(FailureCode code, std::string_view text) { throw EvidenceFailure{diagnostic(code, text)}; }
template<class F> auto protect(F&& function) noexcept -> decltype(function()) {
    return guarded([&]() -> decltype(function()) { try { return function(); } catch (const EvidenceFailure& failure) { return failure.error; } });
}
std::string_view event_name(RetentionEventKind kind) {
    switch (kind) {
        case RetentionEventKind::Allocated: return "allocated";
        case RetentionEventKind::HoldAcquired: return "hold_acquired";
        case RetentionEventKind::HoldReleased: return "hold_released";
        case RetentionEventKind::LongRetention: return "long_retention";
        case RetentionEventKind::OutstandingAtClose: return "outstanding_at_close";
    }
    fail(FailureCode::EvidenceIO, "unknown retention event kind");
}
std::string_view label(const std::array<char, 64>& value) {
    const auto end = std::find(value.begin(), value.end(), '\0'); return {value.data(), static_cast<std::size_t>(end - value.begin())};
}
}
struct FileEvidence::Data {
    Reservation charge;
    std::shared_ptr<ResourceAccount> account;
    RetentionLogConfiguration specification;
    std::string run, clock;
    std::vector<std::string> producers;
    FileEvidenceLimits limits;
    std::string record;
    int fd = -1;
    bool opened = false, finalized = false;
    std::uint64_t bytes = 0, events = 0;
    Sha256 digest{};
    std::optional<Diagnostic> failure;
    ~Data() noexcept { if (fd >= 0) { static_cast<void>(::close(fd)); } }
    void append(std::string_view value) {
        if (value.size() > limits.record_bytes - record.size()) { fail(FailureCode::EvidenceQuota, "retention record exceeds explicit bound"); }
        record.append(value);
    }
    void number(std::uint64_t value) { std::array<char, 32> text{}; const auto result = std::to_chars(text.data(), text.data() + text.size(), value); append({text.data(), static_cast<std::size_t>(result.ptr - text.data())}); }
    void quoted(std::string_view value) {
        checked(validate_text(value, limits.record_bytes, true)); append("\"");
        constexpr std::string_view hex = "0123456789abcdef";
        for (const auto c : value) {
            const auto byte = static_cast<unsigned char>(c);
            if (c == '"') { append("\\\""); } else if (c == '\\') { append("\\\\"); }
            else if (byte < 0x20) { const std::array<char, 6> escaped{'\\', 'u', '0', '0', hex[byte >> 4], hex[byte & 15]}; append({escaped.data(), escaped.size()}); }
            else { append({&c, 1}); }
        }
        append("\"");
    }
    Result<void> open() noexcept {
        return protect([&]() -> Result<void> {
            if (opened || finalized || failure) { return diagnostic(FailureCode::EvidenceIO, "evidence open is single-use"); }
            opened = true;
            fd = ::open(specification.path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
            if (fd < 0) { failure = diagnostic(FailureCode::EvidenceIO, "could not exclusively create required evidence"); return *failure; }
            return ok();
        });
    }
    Result<void> write(const RetentionEvent& event) noexcept {
        if (fd < 0 || finalized) { return diagnostic(FailureCode::EvidenceIO, "evidence is not open"); }
        auto result = protect([&]() -> Result<void> {
            if (failure) { return *failure; }
            if (fd < 0 || finalized) { return diagnostic(FailureCode::EvidenceIO, "evidence is not open"); }
            if (events == specification.max_events) { return diagnostic(FailureCode::EvidenceQuota, "retention event limit exceeded"); }
            record.clear(); append("version=1 event="); quoted(event_name(event.kind)); append(" run="); quoted(run);
            append(" clock="); quoted(clock); append(" step="); number(event.step); append(" allocation="); number(event.allocation);
            append(" producer=");
            if (event.owner) {
                if (event.owner->producer >= producers.size()) { return diagnostic(FailureCode::EvidenceIO, "retention producer is undeclared"); }
                quoted(producers[event.owner->producer]);
            } else { quoted("external_unknown"); }
            append(" publication=");
            if (event.owner) {
                append("\"");
                // Clock names are escaped using the same quoted-string routine;
                // append the sequence inside that bounded string representation.
                const auto start = record.size(); quoted(clock); record.erase(start, 1); record.pop_back(); append(":"); number(event.owner->publication); append("\"");
            } else { quoted("unassigned"); }
            append(" bytes="); number(event.bytes); append(" age_steps="); number(event.age_steps);
            append(" holders="); quoted(label(event.holder)); append(" reason="); quoted(label(event.reason)); append("\n");
            if (record.size() > specification.max_bytes - bytes) { return diagnostic(FailureCode::EvidenceQuota, "retention byte limit exceeded"); }
            std::size_t offset = 0;
            while (offset < record.size()) {
                const auto written = ::write(fd, record.data() + offset, record.size() - offset);
                if (written < 0 && errno == EINTR) { continue; }
                if (written <= 0) { return diagnostic(FailureCode::EvidenceIO, "required retention write failed"); }
                offset += static_cast<std::size_t>(written);
            }
            bytes += record.size(); ++events; return ok();
        });
        if (const auto* error = std::get_if<Diagnostic>(&result); error && !failure) { failure = *error; }
        return result;
    }
    Result<void> finalize() noexcept {
        if (finalized || fd < 0) { return diagnostic(FailureCode::EvidenceIO, "evidence cannot be finalized"); }
        auto result = protect([&]() -> Result<void> {
            if (finalized || fd < 0) { return diagnostic(FailureCode::EvidenceIO, "evidence cannot be finalized"); }
            if (failure) { return *failure; }
            struct stat before{};
            if (::fstat(fd, &before) != 0 || before.st_size < 0 || static_cast<std::uint64_t>(before.st_size) != bytes) { return diagnostic(FailureCode::EvidenceIO, "retention length changed before close"); }
            const auto workspace = checked(checked_add(4096, checked(checked_multiply(bytes, 3))));
            if (workspace > limits.digest_scratch_bytes) { return diagnostic(FailureCode::EvidenceQuota, "evidence digest exceeds explicit workspace"); }
            auto scratch = checked(account->reserve(ResourceKind::Scratch, workspace));
            if (bytes > std::vector<std::byte>{}.max_size()) { return diagnostic(FailureCode::EvidenceQuota, "evidence byte count cannot be represented"); }
            std::vector<std::byte> input(static_cast<std::size_t>(bytes)); std::size_t offset = 0;
            while (offset < input.size()) {
                const auto count = ::pread(fd, input.data() + offset, input.size() - offset, static_cast<off_t>(offset));
                if (count < 0 && errno == EINTR) { continue; }
                if (count <= 0) { return diagnostic(FailureCode::EvidenceIO, "could not validate completed evidence bytes"); }
                offset += static_cast<std::size_t>(count);
            }
            const auto hex = quant::storage::files::sha256(input);
            for (std::size_t i = 0; i < digest.size(); ++i) {
                unsigned value = 0; const auto parsed = std::from_chars(hex.data() + 2 * i, hex.data() + 2 * i + 2, value, 16);
                if (parsed.ec != std::errc{}) { return diagnostic(FailureCode::EvidenceIO, "evidence digest conversion failed"); } digest[i] = static_cast<std::byte>(value);
            }
            struct stat after{};
            if (::fstat(fd, &after) != 0 || after.st_size != before.st_size || after.st_mtimespec.tv_sec != before.st_mtimespec.tv_sec || after.st_mtimespec.tv_nsec != before.st_mtimespec.tv_nsec) {
                return diagnostic(FailureCode::EvidenceIO, "evidence changed during final validation");
            }
            return ok();
        });
        const auto descriptor = std::exchange(fd, -1);
        if (descriptor >= 0 && ::close(descriptor) != 0 && !std::holds_alternative<Diagnostic>(result)) { result = diagnostic(FailureCode::EvidenceIO, "required evidence close failed"); }
        if (const auto* error = std::get_if<Diagnostic>(&result)) { if (!failure) { failure = *error; } }
        else { finalized = true; }
        return result;
    }
    Result<EvidenceReference> reference() const noexcept {
        return guarded([&]() -> Result<EvidenceReference> {
            if (!finalized || failure) { return diagnostic(FailureCode::EvidenceIO, "required evidence has no finalized reference"); }
            return EvidenceReference{"retention", specification.path, bytes, digest, true};
        });
    }
};
Result<FileEvidence> FileEvidence::create(RetentionLogConfiguration specification, std::string run, std::string clock,
    std::vector<std::string> producers, std::shared_ptr<ResourceAccount> account, FileEvidenceLimits limits) noexcept {
    return protect([&]() -> Result<FileEvidence> {
        if (!account || !std::filesystem::path{specification.path}.is_absolute() || specification.format != "key_value_lines_v1" ||
            specification.open != "create_new" || specification.flush != "on_event" || specification.failure != "stop" || specification.overflow != "stop" ||
            specification.max_bytes == 0 || specification.max_events == 0 || limits.record_bytes == 0) { return diagnostic(FailureCode::InvalidConfig, "unsupported explicit file evidence contract"); }
        auto bytes = checked(checked_add(sizeof(Data) + 1024, limits.record_bytes));
        for (const auto* value : {&specification.path, &run, &clock}) { checked(validate_text(*value, limits.record_bytes, false)); bytes = checked(checked_add(bytes, value->size() + 32)); }
        for (const auto& producer : producers) { checked(validate_text(producer, limits.record_bytes, false)); bytes = checked(checked_add(bytes, sizeof(std::string) + producer.size() + 32)); }
        if (bytes > limits.metadata_bytes || limits.record_bytes > std::string{}.max_size()) { return diagnostic(FailureCode::ResourcePlanExceeded, "file evidence metadata exceeds explicit ceiling"); }
        auto charge = checked(account->reserve(ResourceKind::Evidence, limits.metadata_bytes));
        auto data = std::make_shared<Data>(); data->charge = std::move(charge); data->account = std::move(account);
        data->specification = std::move(specification); data->run = std::move(run); data->clock = std::move(clock); data->producers = std::move(producers);
        data->limits = limits; data->record.reserve(static_cast<std::size_t>(limits.record_bytes)); return FileEvidence{std::move(data)};
    });
}
ExecutionEvidence FileEvidence::callbacks() const {
    return {{}, data_->specification, [data = data_]() { return data->open(); }, [data = data_](const RetentionEvent& event) { return data->write(event); },
        [data = data_](const RunReport&) { return data->finalize(); }, [data = data_]() { return data->reference(); }};
}
Result<EvidenceReference> FileEvidence::reference() const noexcept { return data_->reference(); }
} // namespace lexongine
