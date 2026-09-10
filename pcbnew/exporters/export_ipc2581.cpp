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

/**
 * @file export_ipc2581.cpp
 * The IPC-2581 export job, lifted out of DIALOG_EXPORT_2581 so that it can run in a build with
 * no dialogs.  The dialog and the CLI/API job handler both call this.
 */

#include <exporters/export_ipc2581.h>

#include <map>

#include <wx/filename.h>
#include <wx/ffile.h>
#include <wx/wfstream.h>
#include <wx/zipstrm.h>

#include <board.h>
#include <jobs/job_export_pcb_ipc2581.h>
#include <kiplatform/io.h>
#include <paths.h>
#include <pcb_io/pcb_io_mgr.h>
#include <progress_reporter.h>
#include <project.h>
#include <project/project_file.h>
#include <reporter.h>


bool ExportBoardToIpc2581( JOB_EXPORT_PCB_IPC2581& aJob, BOARD* aBoard,
                           PROGRESS_REPORTER* aProgressReporter, REPORTER* aReporter )
{
    wxCHECK( aBoard, false );
    wxString outPath = aJob.GetFullOutputPath( aBoard->GetProject() );

    if( !PATHS::EnsurePathExists( outPath, true ) )
    {
        if( aReporter )
            aReporter->Report( _( "Failed to create output directory\n" ), RPT_SEVERITY_ERROR );

        return false;
    }

    std::map<std::string, UTF8> props;
    props["units"] = aJob.m_units == JOB_EXPORT_PCB_IPC2581::IPC2581_UNITS::MM ? "mm" : "inch";
    props["sigfig"] = wxString::Format( "%d", aJob.m_precision );
    props["version"] = aJob.m_version == JOB_EXPORT_PCB_IPC2581::IPC2581_VERSION::C ? "C" : "B";
    props["OEMRef"] = aJob.m_colInternalId;
    props["mpn"] = aJob.m_colMfgPn;
    props["mfg"] = aJob.m_colMfg;
    props["dist"] = aJob.m_colDist;
    props["distpn"] = aJob.m_colDistPn;

    if( !aJob.m_mode.IsEmpty() )
        props["mode"] = aJob.m_mode;

    if( !aJob.m_sections.IsEmpty() )
        props["sections"] = aJob.m_sections;

    if( !aJob.m_netNamePolicy.IsEmpty() )
        props["netnames"] = aJob.m_netNamePolicy;

    if( !aJob.m_refDesPolicy.IsEmpty() )
        props["refdes"] = aJob.m_refDesPolicy;

    wxString bomRev = aJob.m_bomRev;

    if( bomRev.IsEmpty() && aBoard->GetProject() )
    {
        const IP2581_BOM& bomSettings = aBoard->GetProject()->GetProjectFile().m_IP2581Bom;
        bomRev = bomSettings.bomRev;

        if( bomRev.IsEmpty() )
            bomRev = bomSettings.schRevision;
    }

    if( !bomRev.IsEmpty() )
        props["bomrev"] = bomRev;

    wxString tempFile = wxFileName::CreateTempFileName( wxS( "pcbnew_ipc" ) );

    try
    {
        IO_RELEASER<PCB_IO> pi( PCB_IO_MGR::FindPlugin( PCB_IO_MGR::IPC2581 ) );
        pi->SetProgressReporter( aProgressReporter );
        pi->SetReporter( aReporter );
        pi->SaveBoard( tempFile, *aBoard, &props );
    }
    catch( const IO_ERROR& ioe )
    {
        if( aReporter )
        {
            aReporter->Report( wxString::Format( _( "Error generating IPC-2581 file '%s'.\n%s" ),
                                                  aJob.m_filename,
                                                  ioe.What() ),
                                RPT_SEVERITY_ERROR );
        }

        wxRemoveFile( tempFile );

        return false;
    }

    if( aJob.m_compress )
    {
        wxFileName tempfn = outPath;
        tempfn.SetExt( FILEEXT::Ipc2581FileExtension );
        wxFileName zipfn = tempFile;
        zipfn.SetExt( "zip" );

        {
            wxFFileOutputStream fnout( zipfn.GetFullPath() );

            // Use a large I/O buffer to improve compatibility with cloud-synced folders.
            // See KIPLATFORM::IO::CLOUD_SYNC_BUFFER_SIZE comment for details.
            if( FILE* fp = fnout.GetFile()->fp() )
                setvbuf( fp, nullptr, _IOFBF, KIPLATFORM::IO::CLOUD_SYNC_BUFFER_SIZE );

            wxZipOutputStream   zip( fnout );
            wxFFileInputStream  fnin( tempFile );

            zip.PutNextEntry( tempfn.GetFullName() );
            fnin.Read( zip );
        }

        wxRemoveFile( tempFile );
        tempFile = zipfn.GetFullPath();
    }

    // If save succeeded, replace the original with what we just wrote
    if( !wxRenameFile( tempFile, outPath ) )
    {
        if( aReporter )
        {
            aReporter->Report( wxString::Format( _( "Error generating IPC-2581 file '%s'.\n"
                                                     "Failed to rename temporary file '%s." ),
                                                  outPath,
                                                  tempFile ),
                                RPT_SEVERITY_ERROR );
        }

        return false;
    }

    aJob.AddOutput( outPath );
    return true;
}


