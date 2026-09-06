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

#include <api/api_job_registry.h>

#include <algorithm>

#include <api/api_server.h>
#include <api/common/events.pb.h>
#include <kiid.h>
#include <widgets/progress_reporter_base.h>

using kiapi::common::types::RunJobResponse;
using kiapi::common::types::JobStatus;
using kiapi::common::commands::GetJobStatusResponse;


namespace
{

/// Forwards a job's progress to the registry entry and the events socket
class API_JOB_PROGRESS_REPORTER : public PROGRESS_REPORTER_BASE
{
public:
    API_JOB_PROGRESS_REPORTER( std::function<void( uint32_t, const wxString& )> aOnProgress ) :
            PROGRESS_REPORTER_BASE( 1 ),
            m_onProgress( std::move( aOnProgress ) ),
            m_lastPercent( 0 )
    {
    }

    void Report( const wxString& aMessage ) override
    {
        PROGRESS_REPORTER_BASE::Report( aMessage );
        updateUI();
    }

    void AdvancePhase() override
    {
        PROGRESS_REPORTER_BASE::AdvancePhase();
        updateUI();
    }

    void SetCurrentProgress( double aProgress ) override
    {
        PROGRESS_REPORTER_BASE::SetCurrentProgress( aProgress );
        updateUI();
    }

    void AdvanceProgress() override
    {
        PROGRESS_REPORTER_BASE::AdvanceProgress();
        updateUI();
    }

    void SetTitle( const wxString& aTitle ) override {}

protected:
    bool updateUI() override
    {
        uint32_t percent = std::clamp( CurrentProgress() / 10, 0, 100 );
        wxString message;
        bool     changed;

        {
            std::lock_guard<std::mutex> guard( m_mutex );
            message = m_rptMessage;
            changed = m_messageChanged;
            m_messageChanged = false;
        }

        if( changed || percent != m_lastPercent )
        {
            m_lastPercent = percent;
            m_onProgress( percent, message );
        }

        return !m_cancelled;
    }

private:
    std::function<void( uint32_t, const wxString& )> m_onProgress;
    uint32_t                                         m_lastPercent;
};


} // namespace


API_JOB_REGISTRY& API_JOB_REGISTRY::Instance()
{
    static API_JOB_REGISTRY instance;
    return instance;
}


API_JOB_REGISTRY::~API_JOB_REGISTRY()
{
    {
        std::lock_guard<std::mutex> lock( m_mutex );
        m_stopWorker = true;
    }

    m_condition.notify_all();

    if( m_worker.joinable() )
        m_worker.join();
}


RunJobResponse API_JOB_REGISTRY::Run( KICAD_API_SERVER* aServer, EXECUTOR aExecutor, bool aAsync,
                                      bool aExclusive )
{
    std::shared_ptr<ENTRY> entry = std::make_shared<ENTRY>();
    entry->Id = KIID().AsStdString();
    entry->Server = aServer;
    entry->Executor = std::move( aExecutor );
    entry->Exclusive = aExclusive;
    entry->Result.set_job_id( entry->Id );

    {
        std::lock_guard<std::mutex> lock( m_mutex );

        // Forget the oldest finished jobs
        while( m_jobs.size() >= MAX_REMEMBERED_JOBS )
        {
            auto oldest = std::find_if( m_jobs.begin(), m_jobs.end(),
                                        []( const auto& aPair )
                                        {
                                            return aPair.second->Finished;
                                        } );

            if( oldest == m_jobs.end() )
                break;

            m_jobs.erase( oldest );
        }

        m_jobs[entry->Id] = entry;
    }

    if( !aAsync )
    {
        // A synchronous job must not overlap an asynchronous one on the shared jobs handler
        WaitForIdle();
        execute( entry );

        std::lock_guard<std::mutex> lock( m_mutex );
        return entry->Result;
    }

    {
        std::lock_guard<std::mutex> lock( m_mutex );
        m_queue.push_back( entry );

        if( !m_worker.joinable() )
            m_worker = std::thread( [this]() { workerLoop(); } );
    }

    m_condition.notify_all();

    RunJobResponse response;
    response.set_status( JobStatus::JS_RUNNING );
    response.set_job_id( entry->Id );
    return response;
}


void API_JOB_REGISTRY::execute( const std::shared_ptr<ENTRY>& aEntry )
{
    publishProgress( aEntry, false );

    RunJobResponse result;

    if( !aEntry->Executor )
    {
        result.set_status( JobStatus::JS_ERROR );
        result.set_message( "Internal error: job has no executor" );
    }
    else
    {
        API_JOB_PROGRESS_REPORTER progress(
                [&]( uint32_t aPercent, const wxString& aMessage )
                {
                    {
                        std::lock_guard<std::mutex> lock( m_mutex );
                        aEntry->Percent = aPercent;
                        aEntry->Description = aMessage.ToUTF8();
                    }

                    publishProgress( aEntry, false );
                } );

        try
        {
            result = aEntry->Executor( progress );
        }
        catch( const std::exception& e )
        {
            result.set_status( JobStatus::JS_ERROR );
            result.set_message( e.what() );
        }
        catch( ... )
        {
            result.set_status( JobStatus::JS_ERROR );
            result.set_message( "unexpected exception" );
        }
    }

    result.set_job_id( aEntry->Id );

    {
        std::lock_guard<std::mutex> lock( m_mutex );
        aEntry->Result = std::move( result );
        aEntry->Percent = 100;
        aEntry->Finished = true;
        aEntry->Executor = nullptr;
    }

    publishProgress( aEntry, true );
}


void API_JOB_REGISTRY::publishProgress( const std::shared_ptr<ENTRY>& aEntry, bool aFinished )
{
    if( !aEntry->Server )
        return;

    kiapi::common::events::Event        event;
    kiapi::common::events::JobProgress& progress = *event.mutable_job_progress();

    {
        std::lock_guard<std::mutex> lock( m_mutex );
        progress.set_job_id( aEntry->Id );
        progress.set_percent( aEntry->Percent );
        progress.set_description( aEntry->Description );
    }

    progress.set_finished( aFinished );
    aEntry->Server->Publish( std::move( event ) );
}


void API_JOB_REGISTRY::workerLoop()
{
    for( ;; )
    {
        std::shared_ptr<ENTRY> entry;

        {
            std::unique_lock<std::mutex> lock( m_mutex );

            m_condition.wait( lock,
                              [&]()
                              {
                                  return m_stopWorker || !m_queue.empty();
                              } );

            if( m_stopWorker )
                return;

            entry = m_queue.front();
            m_queue.pop_front();
            m_running = entry;
        }

        execute( entry );

        {
            std::lock_guard<std::mutex> lock( m_mutex );
            m_running.reset();
        }

        m_condition.notify_all();
    }
}


std::optional<GetJobStatusResponse> API_JOB_REGISTRY::Status( const std::string& aJobId ) const
{
    std::lock_guard<std::mutex> lock( m_mutex );

    auto it = m_jobs.find( aJobId );

    if( it == m_jobs.end() )
        return std::nullopt;

    const ENTRY& entry = *it->second;

    GetJobStatusResponse response;
    response.set_job_id( entry.Id );
    response.set_state( entry.Finished ? kiapi::common::commands::JOB_STATE_FINISHED
                                       : kiapi::common::commands::JOB_STATE_RUNNING );
    response.set_percent( entry.Percent );
    response.set_description( entry.Description );

    if( entry.Finished )
        *response.mutable_result() = entry.Result;

    return response;
}


void API_JOB_REGISTRY::WaitForIdle()
{
    std::unique_lock<std::mutex> lock( m_mutex );

    m_condition.wait( lock,
                      [&]()
                      {
                          return m_queue.empty() && !m_running;
                      } );
}


bool API_JOB_REGISTRY::Busy() const
{
    std::lock_guard<std::mutex> lock( m_mutex );
    return !m_queue.empty() || m_running != nullptr;
}


std::optional<std::string> API_JOB_REGISTRY::ExclusiveJob() const
{
    std::lock_guard<std::mutex> lock( m_mutex );

    if( m_running && m_running->Exclusive )
        return m_running->Id;

    for( const std::shared_ptr<ENTRY>& entry : m_queue )
    {
        if( entry->Exclusive )
            return entry->Id;
    }

    return std::nullopt;
}
