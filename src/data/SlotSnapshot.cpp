#include "data/SlotSnapshot.h"

#include "bridge/JcApi.h"
#include "bridge/SlaveTatsNg.h"
#include "core/Config.h"
#include "core/Log.h"
#include "core/Tasks.h"

namespace stm::data
{
    namespace
    {
        bool SameActor(const RE::ActorHandle& a_lhs, const RE::ActorHandle& a_rhs)
        {
            const auto lhsPtr = a_lhs.get();
            const auto rhsPtr = a_rhs.get();
            return lhsPtr.get() == rhsPtr.get();
        }
    }

    SlotSnapshot& SlotSnapshot::Get()
    {
        static SlotSnapshot s_snapshot;
        return s_snapshot;
    }

    void SlotSnapshot::Request(RE::ActorHandle a_actor, std::string a_area)
    {
        const auto now = std::chrono::steady_clock::now();

        std::uint64_t seq = 0;
        {
            std::scoped_lock lock(_mutex);

            if (_inFlight) {
                // The posted build has not run yet. Re-posting one per frame used
                // to flood the task queue (and with it JContainers) whenever it
                // lagged, so wait for it - unless it never arrives at all.
                if ((now - _lastRequest) < std::chrono::seconds(5)) {
                    return;
                }
                _inFlight = false;
            }

            const bool changed = !SameActor(_reqHandle, a_actor) || _reqArea != a_area;
            const auto interval = std::chrono::milliseconds(std::max(50, Config::Get().slotRefreshMs));
            const bool stale = !_valid || (now - _lastRequest) > interval;

            if (!changed && !stale) {
                return;
            }

            _reqHandle = a_actor;
            _reqArea = a_area;
            _lastRequest = now;
            _inFlight = true;
            seq = ++_requestSeq;
        }

        PostToGameThread([a_actor, a_area, seq]() { SlotSnapshot::Get().Build(a_actor, a_area, seq); });
    }

    void SlotSnapshot::Build(RE::ActorHandle a_actor, const std::string& a_area, std::uint64_t a_seq)
    {
        // Breadcrumb: this runs on the game's task queue, one frame after the
        // panel asked for it, so a fault here is reported by the log simply
        // stopping after the frame that requested it.
        log::Stage("task:slots:start");

        std::vector<SlotInfo> slots;

        const auto actorPtr = a_actor.get();
        RE::Actor* actor = actorPtr.get();

        const int  maxSlots = ng::MaxSlots(a_area);
        const bool jcReady = jc::Ready();
        const bool ngReady = actor != nullptr && ng::Ready();

        if (!ngReady) {
            ng::EnsureReady("slot-snapshot");
        }

        // What another overlay mod (RaceMenu's tattoo page, ...) put in this
        // area. The ledger never records those, so without this every slot they
        // hold looks empty - and applying into one would silently overwrite
        // that mod's tattoo.
        std::vector<int> external;
        log::Stage("task:slots:external");
        const bool       haveExternal = ngReady && ng::ExternalSlots(actor, a_area, external);

        slots.reserve(static_cast<std::size_t>(std::max(0, maxSlots)));

        log::Stage("task:slots:ledger");

        for (int i = 0; i < maxSlots; ++i) {
            SlotInfo info;
            info.slot = i;

            if (ngReady) {
                const auto handle = ng::GetAppliedInSlot(actor, a_area, i);
                if (handle > 0) {
                    info.occupied = true;

                    if (jcReady) {
                        const jc::ObjId obj{ handle };
                        info.name = jc::MapGetStr(obj, "name");
                        info.section = jc::MapGetStr(obj, "section");
                        info.color = static_cast<std::uint32_t>(jc::MapGetInt(obj, "color", 0));
                        info.alpha = 1.0f - jc::MapGetFlt(obj, "invertedAlpha", 0.0f);
                        info.glow = static_cast<std::uint32_t>(jc::MapGetInt(obj, "glow", 0));
                    }

                    // A slot we cannot name is either an overlay owned by another
                    // mod or a tattoo whose data we cannot read. Only used as a
                    // fallback: upstream answered the question above when it could.
                    if (info.name.empty()) {
                        info.external = true;
                        info.name = jcReady ? "<external>" : "<unreadable>";
                    }
                }

                // Authoritative, and it also covers slots the ledger knows
                // nothing about. Takes precedence over the name heuristic.
                if (haveExternal && std::find(external.begin(), external.end(), i) != external.end()) {
                    info.occupied = true;
                    info.external = true;
                    if (info.name.empty()) {
                        info.name = "<external>";
                    }
                }
            }

            slots.push_back(std::move(info));
        }

        std::scoped_lock lock(_mutex);

        // A newer request was posted while this one was running: publishing now
        // would label fresh slots with the old area and stop the panel from
        // asking again for a whole refresh interval. Leave _inFlight to the
        // newer build, which owns it.
        if (a_seq != _requestSeq) {
            return;
        }

        _slots = std::move(slots);
        _area = a_area;
        _actorFormID = actor ? actor->GetFormID() : 0;
        _valid = true;
        _inFlight = false;
        ++_revision;
        log::Stage("task:slots:done");
    }

    SlotSnapshot::View SlotSnapshot::Copy() const
    {
        std::scoped_lock lock(_mutex);

        View view;
        view.slots = _slots;
        view.area = _area;
        view.actorFormID = _actorFormID;
        view.revision = _revision;
        view.valid = _valid;
        return view;
    }

    void SlotSnapshot::Invalidate()
    {
        std::scoped_lock lock(_mutex);
        _valid = false;
    }
}
