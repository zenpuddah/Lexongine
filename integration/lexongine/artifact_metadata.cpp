#include "artifact_metadata.hpp"
#include "native_files.hpp"
namespace lexongine {
namespace {
using namespace wire;
std::uint64_t number(std::span<const std::byte> &bytes, std::size_t width) {
    if (bytes.size() < width) {
        fail("truncated canonical identity frame");
    }
    std::uint64_t value = 0;
    for (auto b : bytes.first(width)) {
        value = (value << 8) | std::to_integer<unsigned>(b);
    }
    bytes = bytes.subspan(width);
    return value;
}
std::span<const std::byte> body(std::span<const std::byte> bytes, std::uint16_t kind) {
    constexpr std::string_view domain = "lexongine.identity.v1";
    if (number(bytes, 8) != domain.size() || bytes.size() < domain.size() ||
        !std::ranges::equal(bytes.first(domain.size()),
                            std::as_bytes(std::span{domain.data(), domain.size()}))) {
        fail("invalid canonical identity domain");
    }
    bytes = bytes.subspan(domain.size());
    if (number(bytes, 2) != kind) {
        fail("invalid canonical identity kind");
    }
    return bytes;
}
template <std::size_t N>
std::array<std::span<const std::byte>, N> frames(std::span<const std::byte> bytes) {
    std::array<std::span<const std::byte>, N> result{};
    for (std::size_t i = 0; i < N; ++i) {
        if (number(bytes, 2) != i + 1) {
            fail("canonical identity fields are missing or reordered");
        }
        const auto size = number(bytes, 8);
        if (size > bytes.size()) {
            fail("canonical identity field exceeds remaining bytes");
        }
        result[i] = bytes.first(static_cast<std::size_t>(size));
        bytes = bytes.subspan(static_cast<std::size_t>(size));
    }
    if (!bytes.empty()) {
        fail("canonical identity has trailing fields");
    }
    return result;
}
void identity_consistency(const ArtifactManifest &m, std::shared_ptr<ResourceAccount> account,
                          const MetadataLimits &limits) {
    const auto fields = frames<5>(body(m.canonical_identity, 2));
    const auto computation = frames<14>(fields[0]);
    auto d = checked(Document::create(account, limits.xml));
    auto same = [&](std::span<const std::byte> actual, Element *expected) {
        const auto encoded = canonical(*expected, 0, limits.xml.bytes);
        if (!std::ranges::equal(actual, body(encoded, 0))) {
            fail("manifest projection differs from canonical identity");
        }
    };
    same(fields[1], project(d, m.processing_scope));
    auto *types = d.list();
    for (const auto &t : m.output_types) {
        d.item(types, project(d, t));
    }
    same(fields[2], types);
    same(fields[3], project(d, m.codec));
    same(fields[4], project(d, m.validation));
    same(computation[5], project(d, m.initialization));
    same(computation[6], project(d, m.processing_scope));
    same(computation[7], d.variant("count", d.record("CountHistoryV1",
                                                     {{"preceding", d.u64(m.history.preceding)}})));
}
std::vector<const Element *> list_values(const Element &e) {
    if (std::string_view{e.Name()} != "list") {
        fail("metadata list required");
    }
    attributes(e, {});
    auto v = children(e);
    for (auto *&i : v) {
        if (std::string_view{i->Name()} != "item") {
            fail("metadata list item required");
        }
        attributes(*i, {});
        i = &only_child(*i);
    }
    return v;
}
void child(Document &d, Element *root, std::string_view name, Element *value) {
    auto *e = d.element(name);
    e->InsertEndChild(value);
    root->InsertEndChild(e);
}
std::vector<const Element *> envelope(const Element &root, std::string_view name,
                                      std::initializer_list<std::string_view> expected) {
    if (std::string_view{root.Name()} != name) {
        fail("metadata envelope type mismatch");
    }
    auto cs = children(root);
    if (cs.size() != expected.size()) {
        fail("metadata envelope child count mismatch");
    }
    std::size_t i = 0;
    for (auto n : expected) {
        if (cs[i]->Name() != n) {
            fail("metadata envelope child order mismatch");
        }
        attributes(*cs[i], {});
        cs[i] = &only_child(*cs[i]);
        ++i;
    }
    return cs;
}
Reservation owned(std::string_view xml, std::shared_ptr<ResourceAccount> account,
                  const MetadataLimits &limits) {
    auto required = checked(checked_add(4096, checked(checked_multiply(xml.size(), 2))));
    if (required > limits.owned_bytes) {
        throw Error{diagnostic(FailureCode::ResourcePlanExceeded,
                               "decoded metadata exceeds ownership bound")};
    }
    return checked(account->reserve(ResourceKind::Graph, limits.owned_bytes));
}
void valid_manifest(const ArtifactManifest &m) {
    if (!safe_identifier(m.artifact_id) || m.origin_run_id.empty() ||
        m.identity_digest == Sha256{} || m.payload_digest == Sha256{} ||
        m.processing_scope != m.coverage.expected_scope ||
        m.outcome_count != m.coverage.complete_keys.size() || m.output_types.empty()) {
        fail("manifest identity, scope or counts are inconsistent");
    }
}
void valid_marker(const AcceptedRunRecord &r) {
    if (r.run_id.empty() || !safe_identifier(r.commit_id) ||
        r.cache_status != CachePublicationStatus::Published || r.entries.empty() ||
        r.acceptance_policy_digest == Sha256{} || r.build_identity_digest == Sha256{} ||
        !r.snapshot.finalized || r.snapshot.digest == Sha256{}) {
        fail("accepted record is incomplete");
    }
    for (std::size_t i = 0; i < r.entries.size(); ++i) {
        if (!safe_identifier(r.entries[i].artifact_id) ||
            r.entries[i].manifest_digest == Sha256{} || r.entries[i].payload_digest == Sha256{} ||
            !r.entries[i].manifest_bytes || !r.entries[i].payload_bytes) {
            fail("accepted entry reference is invalid");
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (r.entries[i].artifact_id == r.entries[j].artifact_id) {
                fail("duplicate accepted entry");
            }
        }
    }
    bool snapshot = false;
    for (std::size_t i = 0; i < r.inventory.size(); ++i) {
        const auto &e = r.inventory[i];
        if (!e.finalized || e.name.empty() || !e.absolute_path.starts_with('/') ||
            e.digest == Sha256{}) {
            fail("invalid evidence inventory");
        }
        if (e.absolute_path == r.snapshot.absolute_path) {
            if (e.byte_count != r.snapshot.byte_count || e.digest != r.snapshot.digest) {
                fail("snapshot inventory mismatch");
            }
            snapshot = true;
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (e.name == r.inventory[j].name || e.absolute_path == r.inventory[j].absolute_path) {
                fail("duplicate evidence inventory");
            }
        }
    }
    if (!snapshot) {
        fail("accepted inventory omits snapshot");
    }
}
} // namespace
bool safe_identifier(std::string_view id) noexcept {
    return id.size() == 32 && std::all_of(id.begin(), id.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}
std::span<const std::byte> artifact_computation(const ArtifactManifest &m) {
    auto result = frames<5>(body(m.canonical_identity, 2))[0];
    static_cast<void>(frames<14>(result));
    return result;
}
std::span<const std::byte> computation_body(std::span<const std::byte> bytes) {
    auto result = body(bytes, 1);
    static_cast<void>(frames<14>(result));
    return result;
}
wire::Element *project_entry(wire::Document &d, const EntryReference &e) {
    return d.record("EntryReferenceV1", {{"artifact_id", d.text(e.artifact_id)},
                                         {"identity_digest", d.digest(e.identity_digest)},
                                         {"manifest_digest", d.digest(e.manifest_digest)},
                                         {"manifest_bytes", d.u64(e.manifest_bytes)},
                                         {"payload_digest", d.digest(e.payload_digest)},
                                         {"payload_bytes", d.u64(e.payload_bytes)}});
}
EntryReference decode_entry(const wire::Element &e) {
    auto f = wire::fields(e, "EntryReferenceV1");
    return {wire::text(*f[0]), wire::digest(*f[1]), wire::digest(*f[2]),
            wire::u64(*f[3]),  wire::digest(*f[4]), wire::u64(*f[5])};
}
wire::Element *project_spec(wire::Document &d, const RequiredEvidenceSpec &s) {
    return d.record("RequiredEvidenceSpecV1", {{"name", d.text(s.name)},
                                               {"format", d.text(s.format)},
                                               {"absolute_path", d.text(s.absolute_path)},
                                               {"open_policy", d.text(s.open_policy)},
                                               {"max_bytes", d.u64(s.max_bytes)},
                                               {"max_records", d.u64(s.max_records)},
                                               {"flush_policy", d.text(s.flush_policy)},
                                               {"failure", d.text(s.failure)}});
}
RequiredEvidenceSpec decode_spec(const wire::Element &e) {
    auto f = wire::fields(e, "RequiredEvidenceSpecV1");
    return {wire::text(*f[0]), wire::text(*f[1]), wire::text(*f[2]), wire::text(*f[3]),
            wire::u64(*f[4]),  wire::u64(*f[5]),  wire::text(*f[6]), wire::text(*f[7])};
}
Result<Sha256> coverage_digest(const SourceCoverage &coverage,
                               std::shared_ptr<ResourceAccount> account,
                               const MetadataLimits &limits) noexcept {
    return wire::protect([&]() -> Result<Sha256> {
        auto d = wire::checked(wire::Document::create(account, limits.xml));
        auto *record = wire::project(d, coverage);
        auto bytes = wire::canonical(*record, 4, limits.xml.bytes);
        return files::digest(bytes);
    });
}
Result<wire::EncodedXml> encode_manifest(const ArtifactManifest &m,
                                         std::shared_ptr<ResourceAccount> account,
                                         const MetadataLimits &limits) noexcept {
    return wire::protect([&]() -> Result<wire::EncodedXml> {
        valid_manifest(m);
        identity_consistency(m, account, limits);
        if (checked(files::digest(m.canonical_identity)) != m.identity_digest ||
            checked(coverage_digest(m.coverage, account, limits)) != m.coverage_digest) {
            fail("manifest digest inputs disagree");
        }
        auto d = checked(Document::create(account, limits.xml));
        auto *root = d.element("artifact_manifest");
        d.dom().InsertEndChild(root);
        d.attribute(root, "version", std::uint64_t{1});
        d.attribute(root, "artifact_id", m.artifact_id);
        d.attribute(root, "identity_digest", hex(m.identity_digest));
        d.attribute(root, "origin_run_id", m.origin_run_id);
        d.attribute(root, "codec_id", m.codec.id);
        d.attribute(root, "codec_version", m.codec.version);
        d.attribute(root, "validation_contract_id", m.validation.id);
        d.attribute(root, "validation_contract_version", m.validation.version);
        d.attribute(root, "outcome_count", m.outcome_count);
        d.attribute(root, "payload_bytes", m.payload_bytes);
        d.attribute(root, "payload_digest", hex(m.payload_digest));
        d.attribute(root, "complete_count", m.coverage.complete_keys.size());
        d.attribute(root, "gap_count", m.coverage.gaps.size());
        d.attribute(root, "coverage_digest", hex(m.coverage_digest));
        child(d, root, "canonical_identity", d.text(hex(m.canonical_identity)));
        child(d, root, "processing_scope", project(d, m.processing_scope));
        auto *types = d.list();
        for (const auto &t : m.output_types) {
            d.item(types, project(d, t));
        }
        child(d, root, "ordered_output_types", types);
        child(d, root, "coverage", project(d, m.coverage));
        const Contract history{
            "count",
            1,
            {1, {{"preceding", m.history.preceding, Origin::Derived, "count history"}}}};
        child(d, root, "startup",
              d.record("StartupV1", {{"initialization", project(d, m.initialization)},
                                     {"history", project(d, history)}}));
        return d.render();
    });
}
Result<OwnedMetadata<ArtifactManifest>> decode_manifest(std::string_view xml,
                                                        std::shared_ptr<ResourceAccount> account,
                                                        const MetadataLimits &limits) noexcept {
    return wire::protect([&]() -> Result<OwnedMetadata<ArtifactManifest>> {
        auto charge = owned(xml, account, limits);
        auto d = checked(Document::parse(xml, account, limits.xml));
        const auto &root = d.root();
        attributes(root, {"version", "artifact_id", "identity_digest", "origin_run_id", "codec_id",
                          "codec_version", "validation_contract_id", "validation_contract_version",
                          "outcome_count", "payload_bytes", "payload_digest", "complete_count",
                          "gap_count", "coverage_digest"});
        if (attribute(root, "version") != "1") {
            fail("unsupported manifest version");
        }
        auto cs = envelope(root, "artifact_manifest",
                           {"canonical_identity", "processing_scope", "ordered_output_types",
                            "coverage", "startup"});
        for (auto *c : cs) {
            validate_value(*c);
        }
        ArtifactManifest m;
        m.artifact_id = attribute(root, "artifact_id");
        m.origin_run_id = attribute(root, "origin_run_id");
        m.identity_digest = digest_hex(attribute(root, "identity_digest"));
        m.payload_digest = digest_hex(attribute(root, "payload_digest"));
        m.coverage_digest = digest_hex(attribute(root, "coverage_digest"));
        m.outcome_count = number_attribute<std::uint64_t>(root, "outcome_count");
        m.payload_bytes = number_attribute<std::uint64_t>(root, "payload_bytes");
        m.codec = {std::string{attribute(root, "codec_id")},
                   number_attribute<std::uint32_t>(root, "codec_version"),
                   {1, {}}};
        m.validation = {std::string{attribute(root, "validation_contract_id")},
                        number_attribute<std::uint32_t>(root, "validation_contract_version"),
                        {1, {}}};
        m.canonical_identity = unhex(text(*cs[0]), limits.xml.bytes);
        m.processing_scope = scope(*cs[1]);
        for (auto *t : list_values(*cs[2])) {
            m.output_types.push_back(type(*t));
        }
        m.coverage = coverage(*cs[3]);
        auto startup = fields(*cs[4], "StartupV1");
        m.initialization = initialization(*startup[0]);
        auto history = contract(*startup[1]);
        if (history.id != "count" || history.version != 1 ||
            history.parameters.values.size() != 1 ||
            history.parameters.values[0].name != "preceding" ||
            !std::holds_alternative<std::uint64_t>(history.parameters.values[0].value)) {
            fail("unsupported manifest startup history");
        }
        m.history = {std::get<std::uint64_t>(history.parameters.values[0].value)};
        valid_manifest(m);
        identity_consistency(m, account, limits);
        if (number_attribute<std::uint64_t>(root, "complete_count") !=
                m.coverage.complete_keys.size() ||
            number_attribute<std::uint64_t>(root, "gap_count") != m.coverage.gaps.size() ||
            checked(files::digest(m.canonical_identity)) != m.identity_digest) {
            fail("manifest counts or canonical identity digest mismatch");
        }
        auto coverage_bytes = canonical(*cs[3], 4, limits.xml.bytes);
        if (checked(files::digest(coverage_bytes)) != m.coverage_digest) {
            fail("manifest coverage digest mismatch");
        }
        return OwnedMetadata<ArtifactManifest>{std::move(charge), std::move(m)};
    });
}
Result<wire::EncodedXml> encode_accepted_run(const AcceptedRunRecord &r,
                                             std::shared_ptr<ResourceAccount> account,
                                             const MetadataLimits &limits) noexcept {
    return wire::protect([&]() -> Result<wire::EncodedXml> {
        valid_marker(r);
        auto d = checked(Document::create(account, limits.xml));
        auto *root = d.element("accepted_run");
        d.dom().InsertEndChild(root);
        d.attribute(root, "version", std::uint64_t{1});
        d.attribute(root, "run_id", r.run_id);
        d.attribute(root, "commit_id", r.commit_id);
        d.attribute(root, "numerical", "completed");
        d.attribute(root, "integration_outcome", "completed");
        d.attribute(root, "acceptance", "accepted");
        d.attribute(root, "cache_status", "published");
        d.attribute(root, "acceptance_policy_digest", hex(r.acceptance_policy_digest));
        d.attribute(root, "build_identity_digest", hex(r.build_identity_digest));
        child(d, root, "snapshot_reference",
              d.record("SnapshotReferenceV1", {{"absolute_path", d.text(r.snapshot.absolute_path)},
                                               {"byte_count", d.u64(r.snapshot.byte_count)},
                                               {"digest", d.digest(r.snapshot.digest)}}));
        auto *inventory = d.list();
        for (const auto &e : r.inventory) {
            d.item(inventory, project(d, e));
        }
        child(d, root, "evidence_inventory", inventory);
        auto *entries = d.list();
        for (const auto &e : r.entries) {
            d.item(entries, project_entry(d, e));
        }
        child(d, root, "entries", entries);
        return d.render();
    });
}
Result<OwnedMetadata<AcceptedRunRecord>>
decode_accepted_run(std::string_view xml, std::shared_ptr<ResourceAccount> account,
                    const MetadataLimits &limits) noexcept {
    return wire::protect([&]() -> Result<OwnedMetadata<AcceptedRunRecord>> {
        auto charge = owned(xml, account, limits);
        auto d = checked(Document::parse(xml, account, limits.xml));
        const auto &root = d.root();
        attributes(root, {"version", "run_id", "commit_id", "numerical", "integration_outcome",
                          "acceptance", "cache_status", "acceptance_policy_digest",
                          "build_identity_digest"});
        if (attribute(root, "version") != "1" || attribute(root, "numerical") != "completed" ||
            attribute(root, "integration_outcome") != "completed" ||
            attribute(root, "acceptance") != "accepted" ||
            attribute(root, "cache_status") != "published") {
            fail("record does not prove accepted completed publication");
        }
        auto cs =
            envelope(root, "accepted_run", {"snapshot_reference", "evidence_inventory", "entries"});
        for (auto *c : cs) {
            validate_value(*c);
        }
        auto snapshot = fields(*cs[0], "SnapshotReferenceV1");
        AcceptedRunRecord r{
            std::string{attribute(root, "run_id")},
            std::string{attribute(root, "commit_id")},
            CachePublicationStatus::Published,
            digest_hex(attribute(root, "acceptance_policy_digest")),
            digest_hex(attribute(root, "build_identity_digest")),
            {"snapshot", text(*snapshot[0]), u64(*snapshot[1]), digest(*snapshot[2]), true},
            {},
            {}};
        for (auto *e : list_values(*cs[1])) {
            r.inventory.push_back(evidence(*e));
        }
        for (auto *e : list_values(*cs[2])) {
            r.entries.push_back(decode_entry(*e));
        }
        valid_marker(r);
        return OwnedMetadata<AcceptedRunRecord>{std::move(charge), std::move(r)};
    });
}
} // namespace lexongine
