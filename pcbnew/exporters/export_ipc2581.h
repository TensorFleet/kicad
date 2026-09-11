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

#ifndef EXPORT_IPC2581_H
#define EXPORT_IPC2581_H

class BOARD;
class JOB_EXPORT_PCB_IPC2581;
class PROGRESS_REPORTER;
class REPORTER;

/**
 * Write the IPC-2581 file the job describes, compressing it if the job asks for that, and
 * record the output on the job.  No UI: everything is reported through aReporter.
 *
 * @return true if the file was written
 */
bool ExportBoardToIpc2581( JOB_EXPORT_PCB_IPC2581& aJob, BOARD* aBoard,
                           PROGRESS_REPORTER* aProgressReporter, REPORTER* aReporter );

#endif // EXPORT_IPC2581_H
