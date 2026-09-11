/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef KICAD_API_JOB_REGISTRY_H
#define KICAD_API_JOB_REGISTRY_H

#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <functional>

#include <kicommon.h>
#include <api/common/commands/base_commands.pb.h>
#include <api/common/types/jobs.pb.h>

class KICAD_API_SERVER;
class PROGRESS_REPORTER;

/**
 * Runs jobs on behalf of the IPC API and remembers their results.
 *
 * Lives in kicommon so that every kiface and the host see the same registry.  The work itself is
 * a callback built by the caller (see RunApiJob in api/api_jobs.h), so this class knows nothing
 * about KIWAY or JOB.
 *
 * Synchronous jobs run on the calling thread as before.  Asynchronous jobs (RunJobSettings.async,
 * headless only) are queued on one worker thread, so that at most one job touches the jobs
 * handler's cached document at a time; progress is published as JobProgress events and the
 * result is served by GetJobStatus.  Since 11.0.
 */
class KICOMMON_API API_JOB_REGISTRY
{
public:
    /// Performs the job, reporting progress through the given reporter, and returns its result
    using EXECUTOR = std::function<kiapi::common::types::RunJobResponse( PROGRESS_REPORTER& )>;

    static API_JOB_REGISTRY& Instance();

    /**
     * Run a job.
     * @param aServer publishes the JobProgress events (may be null)
     * @param aAsync queues the job and returns at once with JS_RUNNING; otherwise the response
     *               is the job's result
     * @param aExclusive marks a job that rewrites an open document rather than reading a copy of
     *                   it, so that the editor handlers answer AS_BUSY while it runs
     * @return the job's result, or a JS_RUNNING response carrying the job id (the executor's
     *         result also gets the job id)
     */
    kiapi::common::types::RunJobResponse Run( KICAD_API_SERVER* aServer, EXECUTOR aExecutor, bool aAsync,
                                              bool aExclusive = false );

    /// @return the job's status, or std::nullopt for an unknown id
    std::optional<kiapi::common::commands::GetJobStatusResponse> Status( const std::string& aJobId ) const;

    /// Block until no asynchronous job is queued or running.  Call before a document the jobs
    /// handler may be reading goes away.
    void WaitForIdle();

    /// @return true while an asynchronous job is queued or running
    bool Busy() const;

    /// @return the id of the exclusive job that is queued or running, if there is one.  Such a
    ///         job rewrites an open document, so no other command may touch it meanwhile.
    std::optional<std::string> ExclusiveJob() const;

    /**
     * Run asynchronous jobs on the caller's thread instead of a worker.
     *
     * For hosts that have no threads to give (the headless/wasm build).  An async Run()
     * still registers the job and still answers JS_RUNNING with the job id, so the
     * protocol is unchanged; the job is held until the host next calls RunDeferred(),
     * which a request/response host does between requests.  The worker thread is never
     * created.
     *
     * Deferring rather than running the job inside Run() is what lets a client subscribe
     * to JobProgress after it has been told JS_RUNNING and still see the events, the way
     * it does over a socket.
     *
     * Set this before the first Run().
     */
    void SetInlineMode( bool aInline );

    /// @return true when asynchronous jobs are run on the caller's thread
    bool InlineMode() const;

    /**
     * Run every job that inline mode is holding, on the calling thread.
     *
     * A host calls this when it is between requests -- after a reply has gone out and
     * before the next one is dispatched -- so that the progress events land where a
     * client can hear them.  Does nothing when inline mode is off, and does nothing
     * re-entrantly (a job that dispatches is not re-entered).
     */
    void RunDeferred();

    ~API_JOB_REGISTRY();

private:
    API_JOB_REGISTRY() = default;

    struct ENTRY
    {
        std::string                          Id;
        KICAD_API_SERVER*                    Server = nullptr;
        EXECUTOR                             Executor;
        bool                                 Exclusive = false;
        bool                                 Finished = false;
        uint32_t                             Percent = 0;
        std::string                          Description;
        kiapi::common::types::RunJobResponse Result;
    };

    /// Runs the job now, on the calling thread, and stores the result in the entry
    void execute( const std::shared_ptr<ENTRY>& aEntry );

    void publishProgress( const std::shared_ptr<ENTRY>& aEntry, bool aFinished );

    void workerLoop();

    /// Locked read of m_inlineMode, for use from Run()
    bool inlineMode() const;

    mutable std::mutex                             m_mutex;
    std::condition_variable                        m_condition;
    std::map<std::string, std::shared_ptr<ENTRY>>  m_jobs;
    std::deque<std::shared_ptr<ENTRY>>             m_queue;
    std::shared_ptr<ENTRY>                         m_running;
    std::thread                                    m_worker;
    bool                                           m_stopWorker = false;
    bool                                           m_inlineMode = false;

    /// Inline mode only: jobs accepted but not yet run.  Counted as queued by Busy() and
    /// ExclusiveJob(), so the AS_BUSY guards behave as they do with a worker.
    std::deque<std::shared_ptr<ENTRY>>             m_deferred;
    bool                                           m_runningDeferred = false;

    /// Finished jobs older than this many are forgotten
    static constexpr size_t MAX_REMEMBERED_JOBS = 64;
};

#endif // KICAD_API_JOB_REGISTRY_H
