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

#ifndef KICAD_API_JOBS_H
#define KICAD_API_JOBS_H

#include <memory>

#include <kiway.h>
#include <api/common/types/jobs.pb.h>

class JOB;
class KICAD_API_SERVER;

/**
 * Run an export job for the IPC API through the shared API_JOB_REGISTRY.
 *
 * @param aServer publishes the JobProgress events (may be null)
 * @param aAsync queues the job on the registry's worker thread and returns at once with
 *               JS_RUNNING and the job id; the result is then available from GetJobStatus.
 *               Only safe without an editor window (the jobs handlers read a frame's document
 *               directly), so callers pass RunJobSettings.async && headless.
 * @param aInlineOutputs reads the output files into RunJobResponse.inline_outputs (files up to
 *                       16 MiB; a directory output contributes the files directly inside it)
 * @return the job's result, or the JS_RUNNING response
 *
 * Since 11.0.
 */
kiapi::common::types::RunJobResponse RunApiJob( KICAD_API_SERVER* aServer, KIWAY* aKiway, KIWAY::FACE_T aFace,
                                                std::unique_ptr<JOB> aJob, bool aAsync, bool aInlineOutputs );

#endif // KICAD_API_JOBS_H
