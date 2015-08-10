#include <algorithm>
#include <thread>

#include "state.hh"

using namespace nix;


void State::makeRunnable(Step::ptr step)
{
    printMsg(lvlChatty, format("step ‘%1%’ is now runnable") % step->drvPath);

    {
        auto step_(step->state.lock());
        assert(step_->created);
        assert(!step->finished);
        assert(step_->deps.empty());
    }

    {
        auto runnable_(runnable.lock());
        runnable_->push_back(step);
    }

    wakeDispatcher();
}


void State::dispatcher()
{
    while (true) {
        printMsg(lvlDebug, "dispatcher woken up");

        auto sleepUntil = doDispatch();

        /* Sleep until we're woken up (either because a runnable build
           is added, or because a build finishes). */
        {
            std::unique_lock<std::mutex> lock(dispatcherMutex);
            printMsg(lvlDebug, format("dispatcher sleeping for %1%s") %
                std::chrono::duration_cast<std::chrono::seconds>(sleepUntil - std::chrono::system_clock::now()).count());
            dispatcherWakeup.wait_until(lock, sleepUntil);
            nrDispatcherWakeups++;
        }
    }

    printMsg(lvlError, "dispatcher exits");
}


system_time State::doDispatch()
{
    auto sleepUntil = system_time::max();

    bool keepGoing;

    do {
        system_time now = std::chrono::system_clock::now();

        /* Copy the currentJobs field of each machine. This is
           necessary to ensure that the sort comparator below is
           an ordering. std::sort() can segfault if it isn't. Also
           filter out temporarily disabled machines. */
        struct MachineInfo
        {
            Machine::ptr machine;
            unsigned int currentJobs;
        };
        std::vector<MachineInfo> machinesSorted;
        {
            auto machines_(machines.lock());
            for (auto & m : *machines_) {
                auto info(m.second->state->connectInfo.lock());
                if (info->consecutiveFailures && info->disabledUntil > now) {
                    if (info->disabledUntil < sleepUntil)
                        sleepUntil = info->disabledUntil;
                    continue;
                }
                machinesSorted.push_back({m.second, m.second->state->currentJobs});
            }
        }

        /* Sort the machines by a combination of speed factor and
           available slots. Prioritise the available machines as
           follows:

           - First by load divided by speed factor, rounded to the
             nearest integer.  This causes fast machines to be
             preferred over slow machines with similar loads.

           - Then by speed factor.

           - Finally by load. */
        sort(machinesSorted.begin(), machinesSorted.end(),
            [](const MachineInfo & a, const MachineInfo & b) -> bool
            {
                float ta = roundf(a.currentJobs / a.machine->speedFactor);
                float tb = roundf(b.currentJobs / b.machine->speedFactor);
                return
                    ta != tb ? ta < tb :
                    a.machine->speedFactor != b.machine->speedFactor ? a.machine->speedFactor > b.machine->speedFactor :
                    a.currentJobs > b.currentJobs;
            });

        /* Find a machine with a free slot and find a step to run
           on it. Once we find such a pair, we restart the outer
           loop because the machine sorting will have changed. */
        keepGoing = false;

        for (auto & mi : machinesSorted) {
            // FIXME: can we lose a wakeup if a builder exits concurrently?
            if (mi.machine->state->currentJobs >= mi.machine->maxJobs) continue;

            auto runnable_(runnable.lock());
            //printMsg(lvlDebug, format("%1% runnable builds") % runnable_->size());

            /* FIXME: we're holding the runnable lock too long
               here. This could be more efficient. */

            for (auto i = runnable_->begin(); i != runnable_->end(); ) {
                auto step = i->lock();

                /* Delete dead steps. */
                if (!step) {
                    i = runnable_->erase(i);
                    continue;
                }

                /* Can this machine do this step? */
                if (!mi.machine->supportsStep(step)) {
                    ++i;
                    continue;
                }

                /* Skip previously failed steps that aren't ready
                   to be retried. */
                {
                    auto step_(step->state.lock());
                    if (step_->tries > 0 && step_->after > now) {
                        if (step_->after < sleepUntil)
                            sleepUntil = step_->after;
                        ++i;
                        continue;
                    }
                }

                /* Make a slot reservation and start a thread to
                   do the build. */
                auto reservation = std::make_shared<MaintainCount>(mi.machine->state->currentJobs);
                i = runnable_->erase(i);

                auto builderThread = std::thread(&State::builder, this, step, mi.machine, reservation);
                builderThread.detach(); // FIXME?

                keepGoing = true;
                break;
            }

            if (keepGoing) break;
        }

    } while (keepGoing);

    return sleepUntil;
}


void State::wakeDispatcher()
{
    { std::lock_guard<std::mutex> lock(dispatcherMutex); } // barrier
    dispatcherWakeup.notify_one();
}