#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>

namespace ui_input_coherence
{
// Evidence only. Invalidating these observations never changes a game option,
// retains a resource, submits work, or claims ownership of a presented frame.
inline std::atomic<uint64_t> resourceEpoch{1};
inline void InvalidateResources() noexcept
{
    resourceEpoch.fetch_add(1, std::memory_order_acq_rel);
}

constexpr uint64_t kMaximumAgeMs = 2500;
constexpr size_t kViewportCapacity = 8;

struct Generation
{
    uintptr_t wrapper = 0;
    uint64_t wrapperGeneration = 0;
    uintptr_t provider = 0;
    uint64_t providerGeneration = 0;
    uint64_t createAttempt = 0;
    uint64_t lifecycle = 0;
    uint64_t resources = 0;
    bool operator==(const Generation&) const = default;
    bool Known() const noexcept
    {
        return wrapper && wrapperGeneration && provider && providerGeneration
            && createAttempt && lifecycle && resources;
    }
};

enum class Kind : uint32_t { eHudless, eAlpha, eColorAlpha };
enum class Failure : uint32_t
{
    eNoTags = 0, eUnframed, eMissingPair, eUnknownGeneration,
    eGenerationChanged, eExpired, eResourceIdentity, eResourceShape,
    eDimensions, eNone,
};

struct Resource
{
    bool observed = false;
    bool active = false;
    bool shapeKnown = false;
    uintptr_t native = 0;
    uintptr_t view = 0;
    uint32_t type = 0;
    uint32_t lifecycle = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t top = 0;
    uint32_t left = 0;
    uint32_t format = 0;
    uint64_t lastSeenTick = 0;
};

struct Snapshot
{
    bool hudless = false;
    bool uiAlpha = false;
    bool uiColorAlpha = false;
    bool dimensionsKnown = false;
    bool dimensionsMatch = false;
    bool frameKnown = false;
    bool resourceIdentitiesKnown = false;
    bool generationCurrent = false;
    bool ready = false;
    uint32_t frame = 0;
    uint32_t hudlessWidth = 0;
    uint32_t hudlessHeight = 0;
    uint32_t uiWidth = 0;
    uint32_t uiHeight = 0;
    uint32_t uiFormat = 0;
    uint64_t oldestAgeMs = 0;
    Failure failure = Failure::eNoTags;
};

struct Viewport
{
    uint32_t id = UINT32_MAX;
    bool frameKnown = false;
    uint32_t frame = 0;
    Generation generation{};
    uint64_t tick = 0;
    std::array<Resource, 3> resources{};
};

// Caller serializes Begin/Observe/Read/Invalidate. Storage is bounded even if a
// host supplies arbitrary viewport IDs. Only the latest observed tagged frame
// per viewport is retained; this is not a current-Present frame association.
class Tracker
{
public:
    void Invalidate(uint32_t viewport = UINT32_MAX) noexcept
    {
        for (auto& entry : entries_)
            if (viewport == UINT32_MAX || entry.id == viewport) entry = {};
    }

    Viewport* Begin(uint32_t viewport, bool framed, uint32_t frame,
        const Generation& generation, uint64_t tick) noexcept
    {
        if (viewport == UINT32_MAX) return nullptr;
        auto* entry = Find(viewport);
        if (!entry)
        {
            entry = &*std::min_element(entries_.begin(), entries_.end(),
                [](const Viewport& a, const Viewport& b) {
                    if ((a.id == UINT32_MAX) != (b.id == UINT32_MAX))
                        return a.id == UINT32_MAX;
                    return a.tick < b.tick;
                });
            *entry = {};
            entry->id = viewport;
        }
        if (entry->generation != generation || entry->frameKnown != framed
            || (framed && entry->frame != frame))
        {
            entry->resources = {};
        }
        entry->frameKnown = framed;
        entry->frame = framed ? frame : 0;
        entry->generation = generation;
        entry->tick = tick;
        return entry;
    }

    void Observe(Viewport& entry, Kind kind, const Resource& resource) noexcept
    {
        entry.resources[static_cast<size_t>(kind)] = resource;
    }

    Snapshot Read(uint32_t viewport, const Generation& generation,
        uint64_t now, uint32_t colorWidth = 0, uint32_t colorHeight = 0,
        uint32_t hudlessFormat = 0, uint32_t colorAlphaFormat = 0) const noexcept
    {
        Snapshot result{};
        const auto* entry = Find(viewport);
        if (!entry) return result;
        result.frameKnown = entry->frameKnown;
        result.frame = entry->frame;
        result.generationCurrent = generation.Known() && entry->generation == generation;
        const auto& hudless = entry->resources[0];
        const auto& alpha = entry->resources[1];
        const auto& colorAlpha = entry->resources[2];
        const auto fresh = [&](const Resource& resource) {
            return resource.active && now >= resource.lastSeenTick
                && now - resource.lastSeenTick <= kMaximumAgeMs;
        };
        result.hudless = fresh(hudless);
        result.uiAlpha = fresh(alpha);
        result.uiColorAlpha = fresh(colorAlpha);
        const Resource* ui = result.uiAlpha ? &alpha
            : result.uiColorAlpha ? &colorAlpha : nullptr;
        if (!generation.Known()) { result.failure = Failure::eUnknownGeneration; return result; }
        if (!result.generationCurrent) { result.failure = Failure::eGenerationChanged; return result; }
        if (!entry->frameKnown) { result.failure = Failure::eUnframed; return result; }
        if (!result.hudless || !ui)
        {
            result.failure = hudless.active && (alpha.active || colorAlpha.active)
                ? Failure::eExpired : Failure::eMissingPair;
            return result;
        }
        result.hudlessWidth = hudless.width;
        result.hudlessHeight = hudless.height;
        result.uiWidth = ui->width;
        result.uiHeight = ui->height;
        result.uiFormat = ui->format;
        result.oldestAgeMs = std::max(now - hudless.lastSeenTick, now - ui->lastSeenTick);
        result.resourceIdentitiesKnown = hudless.native && ui->native
            && (hudless.native != ui->native || hudless.view != ui->view);
        if (!result.resourceIdentitiesKnown) { result.failure = Failure::eResourceIdentity; return result; }
        if (!hudless.shapeKnown || !ui->shapeKnown)
        { result.failure = Failure::eResourceShape; return result; }
        result.dimensionsKnown = hudless.width && hudless.height && ui->width && ui->height;
        result.dimensionsMatch = result.dimensionsKnown && hudless.top == ui->top
            && hudless.left == ui->left && hudless.width == ui->width
            && hudless.height == ui->height
            && (!colorWidth || hudless.width == colorWidth)
            && (!colorHeight || hudless.height == colorHeight)
            && (!hudlessFormat || hudless.format == hudlessFormat)
            && (ui != &colorAlpha || !colorAlphaFormat || ui->format == colorAlphaFormat);
        result.ready = result.dimensionsMatch;
        result.failure = result.ready ? Failure::eNone : Failure::eDimensions;
        return result;
    }

private:
    Viewport* Find(uint32_t viewport) noexcept
    {
        if (viewport == UINT32_MAX) return nullptr;
        for (auto& entry : entries_) if (entry.id == viewport) return &entry;
        return nullptr;
    }
    const Viewport* Find(uint32_t viewport) const noexcept
    {
        if (viewport == UINT32_MAX) return nullptr;
        for (const auto& entry : entries_) if (entry.id == viewport) return &entry;
        return nullptr;
    }
    std::array<Viewport, kViewportCapacity> entries_{};
};
}
