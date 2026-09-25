#ifndef OPENMW_COMPONENTS_SCENEUTIL_BOUNDEDTWOWAYWORK_H
#define OPENMW_COMPONENTS_SCENEUTIL_BOUNDEDTWOWAYWORK_H

#include <algorithm>
#include <cstddef>
#include <exception>
#include <functional>
#include <mutex>
#include <system_error>
#include <thread>

namespace SceneUtil
{
    // One optional helper thread for coarse thread-safe work. There is no FIFO:
    // concurrent users fail open to their serial path. Exceptions are rethrown
    // only after the helper has joined.
    class BoundedTwoWayWork final
    {
    public:
        using RangeTask = std::function<void(std::size_t begin, std::size_t end, bool helper)>;

        static bool run(std::size_t count, std::size_t minimumCount, RangeTask task)
        {
            if (!task || count < (std::max)(std::size_t{ 2 }, minimumCount))
                return false;

            static std::mutex gateMutex;
            std::unique_lock gate(gateMutex, std::try_to_lock);
            if (!gate.owns_lock())
                return false;

            const std::size_t middle = count / 2;
            std::exception_ptr helperError;
            std::exception_ptr callerError;
            std::thread helper;
            try
            {
                helper = std::thread([&] {
                    try { task(middle, count, true); }
                    catch (...) { helperError = std::current_exception(); }
                });
            }
            catch (const std::system_error&)
            {
                return false;
            }

            try { task(0, middle, false); }
            catch (...) { callerError = std::current_exception(); }

            helper.join();
            if (callerError) std::rethrow_exception(callerError);
            if (helperError) std::rethrow_exception(helperError);
            return true;
        }
    };
}

#endif
