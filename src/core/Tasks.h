#pragma once

#include "PCH.h"

namespace stm
{
    // Thread identity. Used once at startup to verify that the menu framework
    // really calls the render callback on the game's main thread, which is the
    // assumption every direct engine access in the UI rests on.
    inline std::uint32_t& LoadThreadId()
    {
        static std::uint32_t s_id = 0;
        return s_id;
    }

    inline std::uint32_t ThisThreadId()
    {
        return static_cast<std::uint32_t>(::GetCurrentThreadId());
    }

    // Result of one user-visible operation, produced on the game thread and
    // consumed by the UI on the next frame. Every action reports its real
    // outcome - nothing here is allowed to claim success without checking.
    struct OpResult
    {
        bool        ok = false;
        bool        deferred = false;  // ledger updated, visual sync postponed
        std::string action;
        std::string target;
        std::string detail;

        static OpResult Ok(std::string a_action, std::string a_target, std::string a_detail = {})
        {
            return OpResult{ true, false, std::move(a_action), std::move(a_target), std::move(a_detail) };
        }

        static OpResult Fail(std::string a_action, std::string a_target, std::string a_detail)
        {
            return OpResult{ false, false, std::move(a_action), std::move(a_target), std::move(a_detail) };
        }

        // The ledger was changed, but the visual update could not run yet:
        // upstream's synchronize_tattoos silently gives up - and deliberately
        // leaves .SlaveTats.updated set - while the actor's 3D is not loaded.
        // Reporting this as plain success would be a lie.
        static OpResult Deferred(std::string a_action, std::string a_target, std::string a_detail)
        {
            return OpResult{ true, true, std::move(a_action), std::move(a_target), std::move(a_detail) };
        }
    };

    // Queues work onto the game's task queue (runs at the end of the frame).
    // All engine / JContainers access must go through here.
    inline void PostToGameThread(std::function<void()> a_fn)
    {
        if (const auto task = SKSE::GetTaskInterface()) {
            task->AddTask(std::move(a_fn));
        } else {
            SKSE::log::error("Tasks: task interface unavailable, dropping job");
        }
    }

    // Mailbox between the task queue and the UI.
    //
    // The task queue is NOT guaranteed to be the same thread the menu framework
    // renders on (in practice they differ: the game's main thread publishes the
    // render callback while SKSE runs tasks on another of its threads), so this
    // is genuinely cross-thread and must stay locked. Same for SlotSnapshot.
    class ResultBoard
    {
    public:
        static ResultBoard& Get()
        {
            static ResultBoard s_board;
            return s_board;
        }

        void Push(OpResult a_result)
        {
            std::scoped_lock lock(_mutex);
            _pending.push_back(std::move(a_result));
        }

        // Moves everything queued so far into a_out.
        void Drain(std::vector<OpResult>& a_out)
        {
            std::scoped_lock lock(_mutex);
            if (_pending.empty()) {
                return;
            }
            a_out.insert(a_out.end(), std::make_move_iterator(_pending.begin()), std::make_move_iterator(_pending.end()));
            _pending.clear();
        }

    private:
        std::vector<OpResult> _pending;
        std::mutex            _mutex;
    };
}
