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

#include <api/api_jobs.h>

#include <fmt/format.h>
#include <wx/dir.h>
#include <wx/ffile.h>
#include <wx/filename.h>

#include <api/api_job_registry.h>
#include <jobs/job.h>
#include <reporter.h>

using kiapi::common::types::RunJobResponse;
using kiapi::common::types::JobStatus;


namespace
{

/// Outputs larger than this are not returned inline
constexpr size_t MAX_INLINE_OUTPUT = 16 * 1024 * 1024;


/// Read a job output into the response if it is a file of reasonable size; a directory
/// contributes the files directly inside it
void addInlineOutput( RunJobResponse& aResponse, const wxString& aPath )
{
    auto addFile = [&]( const wxString& aFile )
    {
        wxFFile file( aFile, wxS( "rb" ) );

        if( !file.IsOpened() || file.Length() < 0 || static_cast<size_t>( file.Length() ) > MAX_INLINE_OUTPUT )
            return;

        std::string data;
        data.resize( static_cast<size_t>( file.Length() ) );

        if( file.Length() > 0 && file.Read( data.data(), data.size() ) != static_cast<size_t>( file.Length() ) )
            return;

        kiapi::common::types::JobOutputData* output = aResponse.add_inline_outputs();
        output->set_path( aFile.ToUTF8() );
        output->set_data( std::move( data ) );
    };

    if( wxFileName::DirExists( aPath ) )
    {
        wxDir dir( aPath );

        if( !dir.IsOpened() )
            return;

        wxString name;

        for( bool more = dir.GetFirst( &name, wxEmptyString, wxDIR_FILES ); more; more = dir.GetNext( &name ) )
            addFile( wxFileName( aPath, name ).GetFullPath() );
    }
    else if( wxFileName::FileExists( aPath ) )
    {
        addFile( aPath );
    }
}

} // namespace


RunJobResponse RunApiJob( KICAD_API_SERVER* aServer, KIWAY* aKiway, KIWAY::FACE_T aFace, std::unique_ptr<JOB> aJob,
                          bool aAsync, bool aInlineOutputs )
{
    // The registry outlives the call; the job travels with the executor
    std::shared_ptr<JOB> job = std::move( aJob );

    API_JOB_REGISTRY::EXECUTOR executor =
            [aKiway, aFace, job, aInlineOutputs]( PROGRESS_REPORTER& aProgress ) -> RunJobResponse
            {
                RunJobResponse     result;
                WX_STRING_REPORTER reporter;

                if( !aKiway || !job )
                {
                    result.set_status( JobStatus::JS_ERROR );
                    result.set_message( "Internal error: job has no kiway" );
                    return result;
                }

                int exitCode = aKiway->ProcessJob( aFace, job.get(), &reporter, &aProgress );

                for( const JOB_OUTPUT& output : job->GetOutputs() )
                {
                    result.add_output_path( output.m_outputPath.ToUTF8() );

                    if( aInlineOutputs && exitCode == 0 )
                        addInlineOutput( result, output.m_outputPath );
                }

                if( exitCode == 0 )
                {
                    result.set_status( JobStatus::JS_SUCCESS );
                }
                else
                {
                    result.set_status( JobStatus::JS_ERROR );
                    result.set_message( fmt::format( "Export job '{}' failed with exit code {}: {}", job->GetType(),
                                                     exitCode, reporter.GetMessages().ToStdString() ) );
                }

                return result;
            };

    return API_JOB_REGISTRY::Instance().Run( aServer, std::move( executor ), aAsync );
}
