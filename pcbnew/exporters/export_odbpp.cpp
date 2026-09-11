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
 * @file export_odbpp.cpp
 * The ODB++ export job, lifted out of DIALOG_EXPORT_ODBPP so that it can run in a build with no
 * dialogs.  The one thing it used the parent frame for -- asking whether to overwrite -- is now
 * a callback the dialog supplies and the CLI/API job handler leaves empty.
 */

#include <exporters/export_odbpp.h>

#include <map>

#include <wx/dir.h>
#include <wx/ffile.h>
#include <wx/filename.h>
#include <wx/wfstream.h>
#include <wx/tarstrm.h>
#include <wx/zipstrm.h>
#include <wx/zstream.h>

#include <board.h>
#include <jobs/job_export_pcb_odb.h>
#include <kiplatform/io.h>
#include <locale_io.h>
#include <paths.h>
#include <pcb_io/pcb_io_mgr.h>
#include <progress_reporter.h>
#include <project.h>
#include <reporter.h>
#include <thread_pool.h>
#include <wx_filename.h>


void ExportBoardToOdbpp( const JOB_EXPORT_PCB_ODB& aJob, BOARD* aBoard,
                         const std::function<bool( const wxString& )>& aConfirmOverwrite,
                         PROGRESS_REPORTER* aProgressReporter, REPORTER* aReporter )
{
    LOCALE_IO toggle;

    wxCHECK( aBoard, /* void */ );
    wxString outputPath = aJob.GetFullOutputPath( aBoard->GetProject() );

    if( outputPath.IsEmpty() )
        outputPath = wxFileName( aJob.m_filename ).GetPath();

    wxFileName outputFn( outputPath );

    // Write through symlinks, don't replace them
    WX_FILENAME::ResolvePossibleSymlinks( outputFn );

    if( outputFn.GetPath().IsEmpty() && outputFn.HasName() )
        outputFn.MakeAbsolute();

    bool     outputIsSingleFile = aJob.m_compressionMode != JOB_EXPORT_PCB_ODB::ODB_COMPRESSION::NONE;
    wxString msg;

    if( !PATHS::EnsurePathExists( outputFn.GetFullPath(), outputIsSingleFile ) )
    {
        msg.Printf( _( "Cannot create output directory '%s'." ), outputFn.GetFullPath() );

        if( aReporter )
            aReporter->Report( msg, RPT_SEVERITY_ERROR );

        return;
    }

    if( outputFn.IsDir() && !outputFn.IsDirWritable() )
    {
        msg.Printf( _( "Insufficient permissions to folder '%s'." ), outputFn.GetPath() );

        if( aReporter )
            aReporter->Report( msg, RPT_SEVERITY_ERROR );

        return;
    }

    if( outputIsSingleFile )
    {
        bool writeable = outputFn.FileExists() ? outputFn.IsFileWritable() : outputFn.IsDirWritable();

        if( !writeable )
        {
            msg.Printf( _( "Insufficient permissions to save file '%s'." ), outputFn.GetFullPath() );

            if( aReporter )
                aReporter->Report( msg, RPT_SEVERITY_ERROR );

            return;
        }
    }

    wxFileName tempFile( outputFn.GetFullPath() );

    if( outputIsSingleFile )
    {
        if( outputFn.Exists() )
        {
            if( aConfirmOverwrite )
            {
                msg = wxString::Format( _( "Output files '%s' already exists. Do you want to overwrite it?" ),
                                        outputFn.GetFullPath() );

                if( !aConfirmOverwrite( msg ) )
                    return;

                if( !wxRemoveFile( outputFn.GetFullPath() ) )
                {
                    msg.Printf( _( "Cannot remove existing output file '%s'." ), outputFn.GetFullPath() );

                    if( aReporter )
                        aReporter->Report( msg, RPT_SEVERITY_ERROR );

                    return;
                }
            }
        }

        tempFile.AssignDir( wxFileName::GetTempDir() );
        tempFile.AppendDir( "kicad" );
        tempFile.AppendDir( "odb" );

        if( !wxFileName::Mkdir( tempFile.GetFullPath(), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) )
        {
            msg.Printf( _( "Cannot create temporary output directory." ) );

            if( aReporter )
                aReporter->Report( msg, RPT_SEVERITY_ERROR );

            return;
        }
    }
    else
    {
        // Test for the output directory of tempFile
        wxDir testDir( tempFile.GetFullPath() );

        if( testDir.IsOpened() && ( testDir.HasFiles() || testDir.HasSubDirs() ) )
        {
            if( aConfirmOverwrite )
            {
                msg = wxString::Format( _( "Output directory '%s' already exists and is not empty. "
                                           "Do you want to overwrite it?" ),
                                        tempFile.GetFullPath() );

                if( !aConfirmOverwrite( msg ) )
                    return;

                if( !tempFile.Rmdir( wxPATH_RMDIR_RECURSIVE ) )
                {
                    msg.Printf( _( "Cannot remove existing output directory '%s'." ), tempFile.GetFullPath() );

                    if( aReporter )
                        aReporter->Report( msg, RPT_SEVERITY_ERROR );

                    return;
                }
            }
        }
    }

    std::map<std::string, UTF8> props;

    props["units"] = aJob.m_units == JOB_EXPORT_PCB_ODB::ODB_UNITS::MM ? "mm" : "inch";
    props["sigfig"] = wxString::Format( "%d", aJob.m_precision );

    auto saveFile =
            [&]() -> bool
            {
                try
                {
                    IO_RELEASER<PCB_IO> pi( PCB_IO_MGR::FindPlugin( PCB_IO_MGR::ODBPP ) );
                    pi->SetReporter( aReporter );
                    pi->SetProgressReporter( aProgressReporter );
                    pi->SaveBoard( tempFile.GetFullPath(), *aBoard, &props );
                    return true;
                }
                catch( const IO_ERROR& ioe )
                {
                    if( aReporter )
                    {
                        msg = wxString::Format( _( "Error generating ODBPP files '%s'.\n%s" ),
                                                tempFile.GetFullPath(), ioe.What() );
                        aReporter->Report( msg, RPT_SEVERITY_ERROR );
                    }

                    // In case we started a file but didn't fully write it, clean up
                    wxFileName::Rmdir( tempFile.GetFullPath() );
                    return false;
                }
            };

    thread_pool& tp = GetKiCadThreadPool();
    auto         ret = tp.submit_task( saveFile );

    std::future_status status = ret.wait_for( std::chrono::milliseconds( 250 ) );

    while( status != std::future_status::ready )
    {
        if( aProgressReporter )
            aProgressReporter->KeepRefreshing();

        status = ret.wait_for( std::chrono::milliseconds( 250 ) );
    }

    try
    {
        if( !ret.get() )
            return;
    }
    catch( const std::exception& e )
    {
        if( aReporter )
        {
            aReporter->Report( wxString::Format( "Exception in ODB++ generation: %s", e.what() ),
                               RPT_SEVERITY_ERROR );
        }

        return;
    }

    if( aJob.m_compressionMode == JOB_EXPORT_PCB_ODB::ODB_COMPRESSION::ZIP )
    {
        if( aProgressReporter )
            aProgressReporter->AdvancePhase( _( "Compressing output" ) );

        wxFFileOutputStream fnout( outputFn.GetFullPath() );

        // Use a large I/O buffer to improve compatibility with cloud-synced folders.
        // See KIPLATFORM::IO::CLOUD_SYNC_BUFFER_SIZE comment for details.
        if( FILE* fp = fnout.GetFile()->fp() )
            setvbuf( fp, nullptr, _IOFBF, KIPLATFORM::IO::CLOUD_SYNC_BUFFER_SIZE );

        wxZipOutputStream   zipStream( fnout );

        std::function<void( const wxString&, const wxString& )> addDirToZip =
                [&]( const wxString& dirPath, const wxString& parentPath )
                {
                    wxDir    dir( dirPath );
                    wxString fileName;

                    bool cont = dir.GetFirst( &fileName, wxEmptyString, wxDIR_DEFAULT );

                    while( cont )
                    {
                        wxFileName fileInZip( dirPath, fileName );
                        wxString   relativePath = fileName;

                        if( !parentPath.IsEmpty() )
                            relativePath = parentPath + wxString( wxFileName::GetPathSeparator() ) + fileName;

                        if( wxFileName::DirExists( fileInZip.GetFullPath() ) )
                        {
                            zipStream.PutNextDirEntry( relativePath );
                            addDirToZip( fileInZip.GetFullPath(), relativePath );
                        }
                        else
                        {
                            wxFFileInputStream fileStream( fileInZip.GetFullPath() );
                            zipStream.PutNextEntry( relativePath );
                            fileStream.Read( zipStream );
                        }
                        cont = dir.GetNext( &fileName );
                    }
                };

        addDirToZip( tempFile.GetFullPath(), wxEmptyString );

        zipStream.Close();
        fnout.Close();

        tempFile.Rmdir( wxPATH_RMDIR_RECURSIVE );
    }
    else if( aJob.m_compressionMode == JOB_EXPORT_PCB_ODB::ODB_COMPRESSION::TGZ )
    {
        wxFFileOutputStream fnout( outputFn.GetFullPath() );

        // Use a large I/O buffer to improve compatibility with cloud-synced folders.
        // See KIPLATFORM::IO::CLOUD_SYNC_BUFFER_SIZE comment for details.
        if( FILE* fp = fnout.GetFile()->fp() )
            setvbuf( fp, nullptr, _IOFBF, KIPLATFORM::IO::CLOUD_SYNC_BUFFER_SIZE );

        wxZlibOutputStream  zlibStream( fnout, -1, wxZLIB_GZIP );
        wxTarOutputStream   tarStream( zlibStream );

        std::function<void( const wxString&, const wxString& )> addDirToTar =
                [&]( const wxString& dirPath, const wxString& parentPath )
                {
                    wxDir    dir( dirPath );
                    wxString fileName;

                    bool cont = dir.GetFirst( &fileName, wxEmptyString, wxDIR_DEFAULT );
                    while( cont )
                    {
                        wxFileName fileInTar( dirPath, fileName );
                        wxString   relativePath = fileName;

                        if( !parentPath.IsEmpty() )
                            relativePath = parentPath + wxString( wxFileName::GetPathSeparator() ) + fileName;

                        if( wxFileName::DirExists( fileInTar.GetFullPath() ) )
                        {
                            tarStream.PutNextDirEntry( relativePath );
                            addDirToTar( fileInTar.GetFullPath(), relativePath );
                        }
                        else
                        {
                            wxFFileInputStream fileStream( fileInTar.GetFullPath() );
                            tarStream.PutNextEntry( relativePath, wxDateTime::Now(), fileStream.GetLength() );
                            fileStream.Read( tarStream );
                        }
                        cont = dir.GetNext( &fileName );
                    }
                };

        addDirToTar( tempFile.GetFullPath(),
                     tempFile.GetPath( wxPATH_NO_SEPARATOR ).AfterLast( tempFile.GetPathSeparator() ) );

        tarStream.Close();
        zlibStream.Close();
        fnout.Close();

        tempFile.Rmdir( wxPATH_RMDIR_RECURSIVE );
    }

    if( aProgressReporter )
        aProgressReporter->SetCurrentProgress( 1 );
}
