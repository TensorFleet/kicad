/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef __COROUTINE_SYNC_H
#define __COROUTINE_SYNC_H

/*
 * Run-to-completion stand-in for COROUTINE, used by the headless API core.
 *
 * The real COROUTINE switches stacks with libcontext, which is hand-written
 * assembly with no port for every target the headless core has to build for.
 * The headless core only ever runs tools that never yield -- the RunAction
 * allowlist in pcbnew/api/api_handler_board.cpp admits ZONE_FILLER_TOOL and
 * GLOBAL_EDIT_TOOL, neither of which calls KiYield() -- so Call() can simply
 * invoke the entry point on the caller's stack and return "finished".
 *
 * Anything that does try to suspend throws, loudly and immediately, rather
 * than silently misbehaving: a yield here would need a second stack.
 */

#include <functional>
#include <stdexcept>
#include <utility>

template <typename ReturnType, typename ArgType>
class COROUTINE
{
public:
    COROUTINE() :
        COROUTINE( nullptr )
    {
    }

    /**
     * Create a coroutine from a member method of an object.
     */
    template <class T>
    COROUTINE( T* object, ReturnType ( T::*ptr )( ArgType ) ) :
        COROUTINE( std::bind( ptr, object, std::placeholders::_1 ) )
    {
    }

    /**
     * Create a coroutine from a delegate object.
     */
    COROUTINE( std::function<ReturnType( ArgType )> aEntry ) :
        m_func( std::move( aEntry ) ),
        m_running( false ),
        m_retVal( 0 )
    {
    }

    ~COROUTINE() = default;

    COROUTINE( const COROUTINE& ) = delete;
    COROUTINE& operator=( const COROUTINE& ) = delete;

    /**
     * Stop execution of the coroutine and return control to the caller.
     *
     * There is no caller stack to return to in this build.
     */
    void KiYield()
    {
        throwUnsupported();
    }

    /**
     * KiYield with a value.
     */
    void KiYield( ReturnType& aRetVal )
    {
        throwUnsupported();
    }

    /**
     * Run the requested function on the main stack.
     *
     * We are already on the main stack, but the callers that use this are doing
     * so precisely because they expect to be on a coroutine stack, so refuse
     * rather than quietly changing the semantics.
     */
    void RunMainStack( std::function<void()> func )
    {
        throwUnsupported();
    }

    /**
     * Start execution of a coroutine, passing args as its arguments.
     *
     * @return false always: the entry point has already run to completion.
     */
    bool Call( ArgType aArg )
    {
        run( aArg );
        return Running();
    }

    /**
     * Start execution of a coroutine from within another coroutine.
     */
    bool Call( const COROUTINE& aCor, ArgType aArg )
    {
        run( aArg );
        return Running();
    }

    /**
     * Resume execution of a previously yielded coroutine.
     *
     * Nothing can have yielded, so there is never anything to resume.
     */
    bool Resume()
    {
        return false;
    }

    bool Resume( const COROUTINE& aCor )
    {
        return false;
    }

    /**
     * Return the yielded value (the argument KiYield() was called with).
     */
    const ReturnType& ReturnValue() const
    {
        return m_retVal;
    }

    /**
     * @return true if the coroutine is active.
     */
    bool Running() const
    {
        return m_running;
    }

private:
    [[noreturn]] static void throwUnsupported()
    {
        throw std::runtime_error( "coroutine yield is not supported in the headless build" );
    }

    void run( ArgType aArg )
    {
        if( !m_func )
            return;

        // m_running is what Running() reports.  It is true only for the duration of
        // the call so that a KiYield() deep inside sees a live coroutine, and false
        // afterwards so the caller sees "finished, not suspended".
        m_running = true;

        try
        {
            m_retVal = m_func( aArg );
        }
        catch( ... )
        {
            m_running = false;
            throw;
        }

        m_running = false;
    }

    std::function<ReturnType( ArgType )> m_func;
    bool                                 m_running;
    ReturnType                           m_retVal;
};

#endif // __COROUTINE_SYNC_H
