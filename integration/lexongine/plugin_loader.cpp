#include "lexongine/plugins.hpp"
#include "quant/storage/file_integrity.hpp"

#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <sys/sysctl.h>

namespace lexongine {
namespace {
struct LoaderFailure {
    Diagnostic error;
};
[[noreturn]] void fail(FailureCode code, std::string_view message) {
    throw LoaderFailure{diagnostic(code, message)};
}
[[noreturn]] void fail_name(FailureCode code, std::string_view prefix, std::string_view name) {
    auto error = diagnostic(code, prefix);
    const auto count = std::min(name.size(), error.message.size() - error.length);
    std::copy_n(name.data(), count, error.message.data() + error.length);
    error.length = static_cast<std::uint16_t>(error.length + count);
    error.truncated |= count != name.size();
    throw LoaderFailure{error};
}
template <class T> T take(std::variant<T, Diagnostic> value) {
    if (const auto *error = std::get_if<Diagnostic>(&value)) {
        throw LoaderFailure{*error};
    }
    return std::get<T>(std::move(value));
}
void take(Result<void> value) {
    if (const auto *error = std::get_if<Diagnostic>(&value)) {
        throw LoaderFailure{*error};
    }
}
Sha256 digest(std::string_view hex) {
    if (hex.size() != 64) {
        fail(FailureCode::InvalidConfig, "invalid implementation digest length");
    }
    Sha256 result{};
    auto nibble = [](char c) -> unsigned {
        if (c >= '0' && c <= '9') {
            return static_cast<unsigned>(c - '0');
        }
        if (c >= 'a' && c <= 'f') {
            return static_cast<unsigned>(c - 'a' + 10);
        }
        fail(FailureCode::InvalidConfig, "implementation digest must be lowercase hex");
    };
    for (std::size_t i = 0; i < result.size(); ++i) {
        result[i] = static_cast<std::byte>((nibble(hex[2 * i]) << 4) | nibble(hex[2 * i + 1]));
    }
    return result;
}
std::string read_file(const std::filesystem::path &path, std::shared_ptr<ResourceAccount> account,
                      std::uint64_t maximum, Reservation &reservation) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > maximum) {
        fail(FailureCode::InvalidConfig, "module/manifest file unavailable or over limit");
    }
    auto bytes = take(checked_add(take(checked_multiply(size, 3)), 4096));
    reservation = take(account->reserve(ResourceKind::Scratch, bytes));
    return quant::storage::files::read(path);
}
Sha256 file_digest(const std::filesystem::path &path, std::shared_ptr<ResourceAccount> account,
                   std::uint64_t maximum) {
    Reservation reservation;
    auto bytes = read_file(path, std::move(account), maximum, reservation);
    return digest(quant::storage::files::sha256(bytes));
}
Sha256 build_manifest(std::string_view manifest, std::string_view kind,
                      std::string_view expected_build, const Sha256 &binary) {
    std::array<std::string_view, 4> fields{};
    std::size_t start = 0;
    for (auto &field : fields) {
        const auto end = manifest.find('\n', start);
        if (end == std::string_view::npos) {
            fail(FailureCode::InvalidConfig, "incomplete implementation build manifest");
        }
        field = manifest.substr(start, end - start);
        start = end + 1;
    }
    if (start != manifest.size() || fields[0] != kind) {
        fail(FailureCode::InvalidConfig, "unknown/trailing implementation build manifest data");
    }
    if (fields[1] != expected_build) {
        fail(FailureCode::UnsupportedContract, "manifest SDK/build contract mismatch");
    }
    if (digest(fields[3]) != binary) {
        fail(FailureCode::SourceChanged, "binary differs from its completed build manifest");
    }
    return digest(fields[2]);
}
template <class Function> void commands(std::span<const std::byte> bytes, Function function) {
    if (bytes.size() < sizeof(mach_header_64)) {
        fail(FailureCode::UnsupportedContract, "truncated Mach-O header");
    }
    mach_header_64 header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (header.magic != MH_MAGIC_64 || header.ncmds > 4096 ||
        header.sizeofcmds > bytes.size() - sizeof(header)) {
        fail(FailureCode::UnsupportedContract, "expected bounded native thin 64-bit Mach-O");
    }
    std::size_t offset = sizeof(header);
    const auto end = offset + header.sizeofcmds;
    for (std::uint32_t index = 0; index < header.ncmds; ++index) {
        if (end - offset < sizeof(load_command)) {
            fail(FailureCode::UnsupportedContract, "truncated Mach-O command");
        }
        load_command command{};
        std::memcpy(&command, bytes.data() + offset, sizeof(command));
        if (command.cmdsize < sizeof(command) || command.cmdsize > end - offset) {
            fail(FailureCode::UnsupportedContract, "invalid Mach-O command bound");
        }
        function(command.cmd, bytes.subspan(offset, command.cmdsize));
        offset += command.cmdsize;
    }
    if (offset != end) {
        fail(FailureCode::UnsupportedContract, "Mach-O command count/size mismatch");
    }
}
std::span<const std::byte> image_bytes(const mach_header *image) {
    if (!image || image->magic != MH_MAGIC_64) {
        fail(FailureCode::UnsupportedContract, "unsupported loaded Mach-O image");
    }
    const auto *header = reinterpret_cast<const mach_header_64 *>(image);
    if (header->sizeofcmds > 1048576) {
        fail(FailureCode::UnsupportedContract, "loaded Mach-O metadata exceeds bootstrap bound");
    }
    return {reinterpret_cast<const std::byte *>(image), sizeof(*header) + header->sizeofcmds};
}
std::array<std::byte, 16> uuid(std::span<const std::byte> image) {
    std::array<std::byte, 16> result{};
    bool seen = false;
    commands(image, [&](std::uint32_t kind, std::span<const std::byte> bytes) {
        if (kind != LC_UUID) {
            return;
        }
        if (seen || bytes.size() < sizeof(uuid_command)) {
            fail(FailureCode::UnsupportedContract, "invalid/duplicate image UUID");
        }
        uuid_command command{};
        std::memcpy(&command, bytes.data(), sizeof(command));
        std::memcpy(result.data(), command.uuid, result.size());
        seen = true;
    });
    if (!seen) {
        fail(FailureCode::UnsupportedContract, "loaded implementation requires an image UUID");
    }
    return result;
}
std::string_view dylib_name(std::span<const std::byte> bytes) {
    if (bytes.size() < sizeof(dylib_command)) {
        fail(FailureCode::UnsupportedContract, "truncated dylib command");
    }
    dylib_command command{};
    std::memcpy(&command, bytes.data(), sizeof(command));
    if (command.dylib.name.offset < sizeof(command) || command.dylib.name.offset >= bytes.size()) {
        fail(FailureCode::UnsupportedContract, "invalid dylib name offset");
    }
    const auto *name = reinterpret_cast<const char *>(bytes.data() + command.dylib.name.offset);
    const auto maximum = bytes.size() - command.dylib.name.offset;
    const auto length = ::strnlen(name, maximum);
    if (length == maximum) {
        fail(FailureCode::UnsupportedContract, "unterminated dylib name");
    }
    return {name, length};
}
struct Image {
    const mach_header *header;
    std::string_view path, install;
};
struct NativeModule {
    Reservation metadata_charge;
    void *handle = nullptr;
    ~NativeModule() noexcept {
        if (handle) {
            ::dlclose(handle);
        }
    }
};
struct RegistrationOwner {
    ModulePin module;
    Reservation charge;
};
using Probe = int (*)(char *, std::uint32_t) noexcept;
bool apple_system_path(std::string_view path) noexcept {
    return path.starts_with("/usr/lib/") || path.starts_with("/System/Library/");
}
std::uint32_t dependency_flags(std::uint32_t kind, std::span<const std::byte> bytes) {
    dylib_command legacy{};
    std::memcpy(&legacy, bytes.data(), sizeof(legacy));
    if (legacy.dylib.timestamp == DYLIB_USE_MARKER) {
        if (bytes.size() < sizeof(dylib_use_command)) {
            fail(FailureCode::UnsupportedContract, "truncated dylib use command");
        }
        dylib_use_command command{};
        std::memcpy(&command, bytes.data(), sizeof(command));
        if (command.nameoff < sizeof(command) || (command.flags & ~std::uint32_t{15}) != 0) {
            fail(FailureCode::UnsupportedContract, "unknown dylib use flags/layout");
        }
        return command.flags;
    }
    if (kind == LC_LOAD_WEAK_DYLIB) {
        return DYLIB_USE_WEAK_LINK;
    }
    if (kind == LC_REEXPORT_DYLIB) {
        return DYLIB_USE_REEXPORT;
    }
    if (kind == LC_LOAD_UPWARD_DYLIB) {
        return DYLIB_USE_UPWARD;
    }
    return 0;
}
std::string platform_identity(std::string_view sdk) {
    std::string result = "lexongine.apple-platform-v1\n";
    for (const char *key : {"kern.osversion", "kern.osrelease", "hw.machine"}) {
        std::array<char, 256> value{};
        std::size_t size = value.size();
        if (::sysctlbyname(key, value.data(), &size, nullptr, 0) != 0 || size == 0 ||
            size > value.size() || value[size - 1] != '\0') {
            fail(FailureCode::UnsupportedContract, "cannot obtain bounded Apple platform identity");
        }
        result += key;
        result += '=';
        result.append(value.data(), size - 1);
        result += '\n';
    }
    result += "sdk=";
    result += sdk;
    result += '\n';
    return result;
}

std::vector<NativeDependency> dependencies(const mach_header *root,
                                           std::shared_ptr<ResourceAccount> account,
                                           const PluginLimits &limits) {
    const auto image_count = _dyld_image_count();
    if (image_count > limits.dependency_images) {
        fail(FailureCode::ResourcePlanExceeded, "loaded image inspection limit exceeded");
    }
    std::uint64_t metadata_bytes = take(checked_add(
        8192, take(checked_multiply(image_count,
                                    sizeof(Image) + sizeof(std::uint32_t) * 2 + sizeof(bool)))));
    metadata_bytes = take(
        checked_add(metadata_bytes,
                    take(checked_multiply(limits.dependency_images, sizeof(NativeDependency)))));
    if (metadata_bytes > limits.metadata_bytes) {
        fail(FailureCode::ResourcePlanExceeded, "image inspection metadata limit exceeded");
    }
    std::vector<Image> images;
    images.reserve(image_count);
    for (std::uint32_t i = 0; i < image_count; ++i) {
        const auto *header = _dyld_get_image_header(i);
        const char *path = _dyld_get_image_name(i);
        if (!header || !path) {
            fail(FailureCode::UnsupportedContract,
                 "image inventory changed during synchronous preparation");
        }
        Image image{header, path, {}};
        commands(image_bytes(header), [&](std::uint32_t kind, std::span<const std::byte> bytes) {
            if (kind == LC_ID_DYLIB) {
                image.install = dylib_name(bytes);
            }
        });
        images.push_back(image);
    }
    const auto first = std::find_if(images.begin(), images.end(),
                                    [&](const auto &image) { return image.header == root; });
    if (first == images.end()) {
        fail(FailureCode::UnsupportedContract, "module not present in loaded image inventory");
    }
    std::vector<std::uint32_t> pending;
    pending.reserve(image_count);
    std::vector<bool> visited(image_count, false);
    pending.push_back(static_cast<std::uint32_t>(first - images.begin()));
    visited[pending[0]] = true;
    std::vector<NativeDependency> result;
    result.reserve(static_cast<std::size_t>(limits.dependency_images));
    auto add = [&](std::string_view install, std::string_view path,
                   const std::array<std::byte, 16> &image_uuid, std::optional<Sha256> file,
                   bool present, NativeDependencyKind kind, std::uint32_t flags) {
        metadata_bytes = take(checked_add(metadata_bytes, install.size() + path.size() + 64));
        if (metadata_bytes > limits.metadata_bytes || result.size() >= limits.dependency_images) {
            fail(FailureCode::ResourcePlanExceeded, "dependency metadata limit exceeded");
        }
        result.push_back(
            {std::string{install}, std::string{path}, image_uuid, file, present, kind, flags});
    };
    for (std::size_t cursor = 0; cursor < pending.size(); ++cursor) {
        const auto &image = images[pending[cursor]];
        commands(image_bytes(image.header), [&](std::uint32_t kind,
                                                std::span<const std::byte> bytes) {
            if (kind != LC_LOAD_DYLIB && kind != LC_LOAD_WEAK_DYLIB && kind != LC_REEXPORT_DYLIB &&
                kind != LC_LOAD_UPWARD_DYLIB) {
                return;
            }
            const auto name = dylib_name(bytes);
            const auto flags = dependency_flags(kind, bytes);
            std::optional<std::uint32_t> match;
            for (std::uint32_t i = 0; i < images.size(); ++i) {
                if (images[i].path == name || images[i].install == name) {
                    if (match && *match != i) {
                        fail(FailureCode::UnsupportedContract,
                             "ambiguous loaded dependency identity");
                    }
                    match = i;
                }
            }
            if (!match) {
                if (apple_system_path(name) &&
                    (flags & (DYLIB_USE_WEAK_LINK | DYLIB_USE_DELAYED_INIT)) != 0) {
                    add(name, {}, {}, std::nullopt, false, NativeDependencyKind::AppleDeferredImage,
                        flags);
                    return;
                }
                fail_name(FailureCode::UnsupportedContract, "unresolved loaded dependency: ", name);
            }
            if (!visited[*match]) {
                visited[*match] = true;
                pending.push_back(*match);
            }
        });
        if (image.header == root) {
            continue;
        }
        std::optional<Sha256> file;
        std::error_code error;
        const bool exists =
            std::filesystem::is_regular_file(std::filesystem::path{image.path}, error);
        if (exists) {
            file = file_digest(std::filesystem::canonical(std::filesystem::path{image.path}),
                               account, limits.file_bytes);
        } else if (!apple_system_path(image.path)) {
            fail(FailureCode::SourceChanged, "loaded non-system dependency is missing from disk");
        }
        add(image.install.empty() ? image.path : image.install, image.path,
            uuid(image_bytes(image.header)), file, true,
            file ? NativeDependencyKind::FileContent : NativeDependencyKind::AppleLoadedImage, 0);
    }
    std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
        return std::tie(a.install_name, a.loaded_path, a.present, a.load_flags) <
               std::tie(b.install_name, b.loaded_path, b.present, b.load_flags);
    });
    return result;
}
Sha256 dependency_digest(std::span<const NativeDependency> dependencies,
                         std::shared_ptr<ResourceAccount> account, const PluginLimits &limits,
                         std::string_view platform) {
    auto charge = take(account->reserve(
        ResourceKind::Scratch,
        take(checked_add(take(checked_multiply(limits.metadata_bytes, 3)), 4096))));
    std::string inventory = "lexongine.native-dependencies.v1";
    auto text = [&](std::string_view value) {
        inventory += std::to_string(value.size());
        inventory += ':';
        inventory += value;
    };
    text(platform);
    for (const auto &dependency : dependencies) {
        text(dependency.install_name);
        text(dependency.loaded_path);
        inventory += dependency.present ? '1' : '0';
        inventory += static_cast<char>(dependency.kind);
        for (unsigned i = 0; i < 4; ++i) {
            inventory += static_cast<char>((dependency.load_flags >> ((3 - i) * 8)) & 255);
        }
        inventory.append(reinterpret_cast<const char *>(dependency.image_uuid.data()),
                         dependency.image_uuid.size());
        inventory += dependency.file_digest ? '1' : '0';
        if (dependency.file_digest) {
            inventory.append(reinterpret_cast<const char *>(dependency.file_digest->data()),
                             dependency.file_digest->size());
        }
    }
    return digest(quant::storage::files::sha256(inventory));
}
} // namespace

struct LoadedPlugin::Data {
    ModulePin pin;
    ImplementationIdentity identity;
    std::vector<NativeDependency> dependencies;
    std::string path;
    std::string platform;
};
const ImplementationIdentity &LoadedPlugin::identity() const noexcept {
    return data_->identity;
}
const ModulePin &LoadedPlugin::pin() const noexcept {
    return data_->pin;
}
std::span<const NativeDependency> LoadedPlugin::dependencies() const noexcept {
    return data_->dependencies;
}
std::string_view LoadedPlugin::path() const noexcept {
    return data_->path;
}
std::string_view LoadedPlugin::platform_identity() const noexcept {
    return data_->platform;
}

Result<LoadedPlugin> load_plugin(std::string_view supplied_path,
                                 std::string_view expected_build_contract, Registry &registry,
                                 std::shared_ptr<ResourceAccount> account,
                                 const PluginLimits &limits) noexcept {
    return guarded([&]() -> Result<LoadedPlugin> {
        try {
            if (!account || registry.frozen() || limits.probe_bytes == 0 ||
                limits.probe_bytes > limits.metadata_bytes || expected_build_contract.empty() ||
                expected_build_contract.size() >= limits.probe_bytes ||
                supplied_path.size() > limits.metadata_bytes) {
                fail(FailureCode::InvalidConfig,
                     "invalid explicit plugin preparation limits/phase");
            }
            const auto minimum_metadata =
                take(checked_add(take(checked_multiply(supplied_path.size(), 4)),
                                 take(checked_add(8192, limits.probe_bytes))));
            if (minimum_metadata > limits.metadata_bytes) {
                fail(FailureCode::ResourcePlanExceeded,
                     "plugin bootstrap metadata exceeds explicit budget");
            }
            auto metadata = take(account->reserve(ResourceKind::Graph, limits.metadata_bytes));
            const std::filesystem::path requested{supplied_path};
            if (!requested.is_absolute()) {
                fail(FailureCode::InvalidConfig, "plugin path must be absolute");
            }
            std::error_code error;
            const auto path = std::filesystem::canonical(requested, error);
            if (error) {
                fail(FailureCode::InvalidConfig, "plugin file does not exist");
            }
            Reservation binary_charge;
            auto binary = read_file(path, account, limits.file_bytes, binary_charge);
            const auto binary_hash = digest(quant::storage::files::sha256(binary));
            const auto disk_uuid = uuid(std::as_bytes(std::span{binary.data(), binary.size()}));
            Reservation manifest_charge;
            const auto manifest = read_file(path.string() + ".lexongine-build", account,
                                            limits.metadata_bytes, manifest_charge);
            const auto source_build = build_manifest(manifest, "lexongine-plugin-build-v1",
                                                     expected_build_contract, binary_hash);
            auto module = std::make_shared<NativeModule>();
            module->metadata_charge = std::move(metadata);
            module->handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (!module->handle) {
                fail(FailureCode::UnsupportedContract, "native plugin load failed");
            }
            auto probe = reinterpret_cast<Probe>(::dlsym(module->handle, "lexongine_probe_v1"));
            if (!probe) {
                fail(FailureCode::UnsupportedContract, "missing native plugin probe");
            }
            std::vector<char> buffer(limits.probe_bytes, '\x7f');
            if (probe(buffer.data(), limits.probe_bytes) != 0) {
                fail(FailureCode::UnsupportedContract,
                     "native plugin probe rejected capacity/contract");
            }
            const auto terminator = std::find(buffer.begin(), buffer.end(), '\0');
            if (terminator == buffer.end() ||
                std::string_view{buffer.data(),
                                 static_cast<std::size_t>(terminator - buffer.begin())} !=
                    expected_build_contract) {
                fail(FailureCode::UnsupportedContract,
                     "malformed or mismatched native plugin probe");
            }
            auto entry = reinterpret_cast<Registry::ModuleRegistration>(
                ::dlsym(module->handle, "lexongine_register_v1"));
            if (!entry) {
                fail(FailureCode::UnsupportedContract, "missing native plugin registration entry");
            }
            Dl_info info{};
            if (::dladdr(reinterpret_cast<const void *>(probe), &info) == 0 || !info.dli_fbase ||
                !info.dli_fname || std::filesystem::canonical(info.dli_fname) != path ||
                uuid(image_bytes(static_cast<const mach_header *>(info.dli_fbase))) != disk_uuid) {
                fail(FailureCode::SourceChanged,
                     "loaded image does not match the selected native binary");
            }
            auto data = std::make_shared<LoadedPlugin::Data>();
            data->pin = module;
            data->path = path.string();
            data->platform = platform_identity(expected_build_contract);
            data->dependencies =
                dependencies(static_cast<const mach_header *>(info.dli_fbase), account, limits);
            data->identity = {
                binary_hash, dependency_digest(data->dependencies, account, limits, data->platform),
                source_build, std::string{expected_build_contract}};
            if (file_digest(path, account, limits.file_bytes) != binary_hash) {
                fail(FailureCode::SourceChanged, "native binary changed during loading");
            }
            auto registration_charge =
                take(account->reserve(ResourceKind::Graph, limits.registration_bytes));
            ModulePin registration_pin = std::make_shared<const RegistrationOwner>(
                RegistrationOwner{module, std::move(registration_charge)});
            take(registry.register_module(std::move(registration_pin), data->identity, entry,
                                          account, limits.registration_bytes));
            return LoadedPlugin{std::move(data)};
        } catch (const LoaderFailure &failure) {
            return failure.error;
        }
    });
}

struct HostBuild::Data {
    Reservation charge;
    ImplementationIdentity identity;
    std::vector<NativeDependency> dependencies;
    std::string path, platform;
};
const ImplementationIdentity &HostBuild::identity() const noexcept {
    return data_->identity;
}
std::span<const NativeDependency> HostBuild::dependencies() const noexcept {
    return data_->dependencies;
}
std::string_view HostBuild::path() const noexcept {
    return data_->path;
}
std::string_view HostBuild::platform_identity() const noexcept {
    return data_->platform;
}
Result<HostBuild> inspect_host_build(std::string_view expected_build,
                                     std::string_view expected_source_build,
                                     std::shared_ptr<ResourceAccount> account,
                                     const HostBuildLimits &limits) noexcept {
    return guarded([&]() -> Result<HostBuild> {
        try {
            if (!account || expected_build.empty() ||
                expected_build.size() > limits.metadata_bytes || limits.metadata_bytes < 16384) {
                fail(FailureCode::InvalidConfig,
                     "host build inspection requires explicit bounds and build facts");
            }
            auto charge = take(account->reserve(ResourceKind::Graph, limits.metadata_bytes));
            const auto expected_source = digest(expected_source_build);
            const auto *image = _dyld_get_image_header(0);
            const auto *image_path = _dyld_get_image_name(0);
            if (!image || !image_path ||
                std::string_view{image_path}.size() > limits.metadata_bytes / 4) {
                fail(FailureCode::UnsupportedContract, "cannot inspect bounded current host image");
            }
            const auto path = std::filesystem::canonical(image_path);
            Reservation binary_charge;
            const auto binary = read_file(path, account, limits.file_bytes, binary_charge);
            const auto binary_hash = digest(quant::storage::files::sha256(binary));
            if (uuid(std::as_bytes(std::span{binary.data(), binary.size()})) !=
                uuid(image_bytes(image))) {
                fail(FailureCode::SourceChanged, "host executable differs from its loaded image");
            }
            Reservation manifest_charge;
            const auto manifest = read_file(path.string() + ".lexongine-build", account,
                                            limits.metadata_bytes, manifest_charge);
            const auto source_build =
                build_manifest(manifest, "lexongine-host-build-v1", expected_build, binary_hash);
            if (source_build != expected_source) {
                fail(FailureCode::SourceChanged,
                     "host source/build facts differ from compiled pipeline identity");
            }
            const PluginLimits inspection{0, limits.file_bytes, limits.metadata_bytes, 0,
                                          limits.dependency_images};
            auto platform = platform_identity(expected_build);
            auto inventory = dependencies(image, account, inspection);
            ImplementationIdentity identity{
                binary_hash, dependency_digest(inventory, account, inspection, platform),
                source_build, std::string{expected_build}};
            if (file_digest(path, account, limits.file_bytes) != binary_hash) {
                fail(FailureCode::SourceChanged, "host executable changed during inspection");
            }
            return HostBuild{std::make_shared<const HostBuild::Data>(
                HostBuild::Data{std::move(charge), std::move(identity), std::move(inventory),
                                path.string(), std::move(platform)})};
        } catch (const LoaderFailure &failure) {
            return failure.error;
        }
    });
}
} // namespace lexongine
