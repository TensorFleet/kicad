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

/*
 * "No VCS, no local history" stand-ins for KICAD_HEADLESS_API.
 *
 * KICOMMON_VCS_SRCS, local_history.cpp, history_lock.cpp and text_eval_vcs.cpp are all
 * written directly against libgit2, which is not in the Emscripten dependency set.  This
 * file supplies the handful of entry points the API path still reaches, all behaving as
 * they do for a project that is simply not under version control: no repository, no
 * snapshots, no autosave files.  Saving a document is unaffected -- LOCAL_HISTORY only ever
 * ran *after* the file had been written.
 */

#include <local_history.h>
#include <text_eval/text_eval_vcs.h>

#include <wx/string.h>


LOCAL_HISTORY::LOCAL_HISTORY()
{
}


LOCAL_HISTORY::~LOCAL_HISTORY()
{
}


bool LOCAL_HISTORY::Init( const wxString& aProjectPath )
{
    return false;
}


void LOCAL_HISTORY::RegisterSaver(
        const void* aSaverObject,
        const std::function<void( const wxString&, std::vector<HISTORY_FILE_DATA>& )>& aSaver,
        const std::weak_ptr<void>& aLifetime )
{
}


void LOCAL_HISTORY::UnregisterSaver( const void* aSaverObject )
{
}


void LOCAL_HISTORY::ClearAllSavers()
{
}


void LOCAL_HISTORY::NoteFileChange( const wxString& aFile )
{
}


bool LOCAL_HISTORY::CommitPending()
{
    return false;
}


void LOCAL_HISTORY::WaitForPendingSave()
{
}


bool LOCAL_HISTORY::HistoryExists( const wxString& aProjectPath )
{
    return false;
}


bool LOCAL_HISTORY::TagSave( const wxString& aProjectPath, const wxString& aFileType )
{
    return false;
}


bool LOCAL_HISTORY::HeadNewerThanLastSave( const wxString& aProjectPath )
{
    return false;
}


wxString LOCAL_HISTORY::GetHeadHash( const wxString& aProjectPath )
{
    return wxEmptyString;
}


std::vector<std::pair<wxString, wxString>>
LOCAL_HISTORY::FindStaleAutosaveFiles( const wxString& aProjectPath,
                                       const std::vector<wxString>& aExtensions ) const
{
    return {};
}


void LOCAL_HISTORY::RemoveAutosaveFiles( const wxString& aProjectPath ) const
{
}


bool LOCAL_HISTORY::RunRegisteredSaversAsAutosaveFiles( const wxString& aProjectPath )
{
    return false;
}




/*
 * The ${VCS_*} text variables.  text_eval_parser.cpp calls these for every text evaluation
 * that mentions one; with no repository they answer exactly as they do for a project that is
 * not under version control -- empty strings, a zero distance, and "not dirty".
 */

namespace TEXT_EVAL_VCS
{

std::string GetCommitHash( const std::string& aPath, int aLength )
{
    return {};
}


std::string GetNearestTag( const std::string& aMatch, bool aAnyTags )
{
    return {};
}


int GetDistanceFromTag( const std::string& aMatch, bool aAnyTags )
{
    return 0;
}


bool IsDirty( bool aIncludeUntracked )
{
    return false;
}


std::string GetAuthor( const std::string& aPath )
{
    return {};
}


std::string GetAuthorEmail( const std::string& aPath )
{
    return {};
}


std::string GetCommitter( const std::string& aPath )
{
    return {};
}


std::string GetCommitterEmail( const std::string& aPath )
{
    return {};
}


std::string GetBranch()
{
    return {};
}


int64_t GetCommitTimestamp( const std::string& aPath )
{
    return 0;
}


std::string GetCommitDate( const std::string& aPath )
{
    return {};
}


void SetContextPath( const wxString& aPath )
{
}


wxString GetContextPath()
{
    return wxS( "." );
}


CONTEXT_PATH_SCOPE::CONTEXT_PATH_SCOPE( const wxString& aPath )
{
}


CONTEXT_PATH_SCOPE::~CONTEXT_PATH_SCOPE()
{
}

} // namespace TEXT_EVAL_VCS
