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

#ifndef EXPORT_ODBPP_H
#define EXPORT_ODBPP_H

#include <functional>

#include <wx/string.h>

class BOARD;
class JOB_EXPORT_PCB_ODB;
class PROGRESS_REPORTER;
class REPORTER;

/**
 * Write the ODB++ output the job describes -- a directory, or a zip/tgz archive -- and record
 * the outputs on the job.  Errors are reported through aReporter.
 *
 * @param aConfirmOverwrite is asked before an existing output is replaced; an empty function
 *        leaves the existing output alone, which is what the CLI and the API do.
 */
void ExportBoardToOdbpp( const JOB_EXPORT_PCB_ODB& aJob, BOARD* aBoard,
                         const std::function<bool( const wxString& )>& aConfirmOverwrite,
                         PROGRESS_REPORTER* aProgressReporter, REPORTER* aReporter );

#endif // EXPORT_ODBPP_H
