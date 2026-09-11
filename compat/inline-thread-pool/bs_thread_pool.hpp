/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 3
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

#ifndef BS_INLINE_THREAD_POOL_HPP
#define BS_INLINE_THREAD_POOL_HPP

/*
 * Drop-in replacement for thirdparty/thread-pool's bs_thread_pool.hpp that runs
 * every task on the calling thread, used by the headless API core.
 *
 * This is a facade, not a port: it presents the slice of the BS::thread_pool v5
 * API that KiCad actually uses, with the same names and signatures, so the ~50
 * call sites compile unchanged.  Tasks run to completion inside the submit and
 * detach calls, so the futures handed back are already satisfied and
 * wait()/wait_for()/purge() have nothing left to do.
 *
 * Exceptions keep the same shape as the real pool: a task that throws stores its
 * exception in the returned future, so the caller sees it at .get() rather than
 * at the point of submission.
 *
 * The include directory that provides this header is selected in
 * thirdparty/CMakeLists.txt under KICAD_HEADLESS_API.
 */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace BS
{

using size_t = std::size_t;
using priority_t = std::int8_t;

/**
 * The named priority levels.  Nothing here reorders anything -- tasks run in
 * submission order -- but the constants have to exist for the call sites.
 */
namespace pr
{
constexpr priority_t highest = 127;
constexpr priority_t high = 64;
constexpr priority_t normal = 0;
constexpr priority_t low = -64;
constexpr priority_t lowest = -128;
} // namespace pr

using opt_t = std::uint8_t;

/**
 * Optional-feature flags for the thread_pool template parameter.  The values have
 * to match the real header's: include/singleton.h forward-declares the pool as
 * BS::thread_pool<1> and the two declarations must agree.
 */
enum tp : opt_t
{
    none = 0,
    priority = 1 << 0,
    pause = 1 << 2,
    wait_deadlock_checks = 1 << 3
};


enum class os_thread_priority
{
    idle,
    lowest,
    below_normal,
    normal,
    above_normal,
    highest,
    realtime
};


namespace this_thread
{
/// There is only ever the calling thread, which is not a pool worker.
[[nodiscard]] inline std::optional<::BS::size_t> get_index()
{
    return std::nullopt;
}

[[nodiscard]] inline std::optional<void*> get_pool()
{
    return std::nullopt;
}

inline bool set_os_thread_name( const std::string& )
{
    return false;
}

[[nodiscard]] inline std::optional<os_thread_priority> get_os_thread_priority()
{
    return std::nullopt;
}

inline bool set_os_thread_priority( os_thread_priority )
{
    return false;
}
} // namespace this_thread


/**
 * A vector of futures with the collective waiting helpers.  Identical in shape to
 * the real one; since every future here is already satisfied, the waits return
 * immediately.
 */
template <typename T>
class [[nodiscard]] multi_future : public std::vector<std::future<T>>
{
public:
    using std::vector<std::future<T>>::vector;

    [[nodiscard]] std::conditional_t<std::is_void_v<T>, void, std::vector<T>> get()
    {
        if constexpr( std::is_void_v<T> )
        {
            for( std::future<T>& future : *this )
                future.get();

            return;
        }
        else
        {
            std::vector<T> results;
            results.reserve( this->size() );

            for( std::future<T>& future : *this )
                results.push_back( future.get() );

            return results;
        }
    }

    [[nodiscard]] ::BS::size_t ready_count() const
    {
        ::BS::size_t count = 0;

        for( const std::future<T>& future : *this )
        {
            if( future.valid()
                && future.wait_for( std::chrono::duration<double>::zero() )
                           == std::future_status::ready )
            {
                ++count;
            }
        }

        return count;
    }

    [[nodiscard]] bool valid() const
    {
        for( const std::future<T>& future : *this )
        {
            if( !future.valid() )
                return false;
        }

        return true;
    }

    void wait() const
    {
        for( const std::future<T>& future : *this )
            future.wait();
    }

    template <typename R, typename P>
    bool wait_for( const std::chrono::duration<R, P>& ) const
    {
        return true;
    }

    template <typename C, typename D>
    bool wait_until( const std::chrono::time_point<C, D>& ) const
    {
        return true;
    }
};


/**
 * How the two loop bounds are reconciled into one index type.
 *
 * Taken from the real header verbatim in behaviour: call sites routinely pass a
 * literal 0 (int) together with a container's size() (std::size_t), and the
 * deduction has to pick a type that can hold both rather than failing.
 */
template <typename T1, typename T2, typename Enable = void>
struct common_index_type
{
    using type = std::common_type_t<T1, T2>;
};

template <typename T1, typename T2>
struct common_index_type<T1, T2, std::enable_if_t<std::is_signed_v<T1> && std::is_signed_v<T2>>>
{
    using type = std::conditional_t<( sizeof( T1 ) >= sizeof( T2 ) ), T1, T2>;
};

template <typename T1, typename T2>
struct common_index_type<T1, T2, std::enable_if_t<std::is_unsigned_v<T1> && std::is_unsigned_v<T2>>>
{
    using type = std::conditional_t<( sizeof( T1 ) >= sizeof( T2 ) ), T1, T2>;
};

template <typename T1, typename T2>
struct common_index_type<T1, T2,
                         std::enable_if_t<( std::is_signed_v<T1> && std::is_unsigned_v<T2> )
                                          || ( std::is_unsigned_v<T1> && std::is_signed_v<T2> )>>
{
    using S = std::conditional_t<std::is_signed_v<T1>, T1, T2>;
    using U = std::conditional_t<std::is_unsigned_v<T1>, T1, T2>;

    static constexpr std::size_t larger_size = ( sizeof( S ) > sizeof( U ) ) ? sizeof( S )
                                                                             : sizeof( U );

    using type = std::conditional_t<
            larger_size <= 4,
            std::conditional_t<
                    larger_size == 1 || ( sizeof( S ) == 2 && sizeof( U ) == 1 ), std::int16_t,
                    std::conditional_t<larger_size == 2 || ( sizeof( S ) == 4 && sizeof( U ) < 4 ),
                                       std::int32_t, std::int64_t>>,
            std::conditional_t<sizeof( U ) == 8, std::uint64_t, std::int64_t>>;
};

template <typename T1, typename T2>
using common_index_type_t = typename common_index_type<T1, T2>::type;


/**
 * Splits [first, index_after_last) into blocks.  Kept because submit_blocks()
 * callers receive the block bounds and the real class is part of the API.
 */
template <typename T>
class [[nodiscard]] index_range
{
public:
    index_range( const T first, const T index_after_last, const std::size_t num_blocks ) :
            m_first( first ),
            m_index_after_last( index_after_last )
    {
        if( index_after_last > first )
        {
            const ::BS::size_t total =
                    static_cast<::BS::size_t>( index_after_last - first );

            m_blocks = num_blocks > 0 ? ( num_blocks < total ? num_blocks : total ) : 1;
            m_size = total;
        }
    }

    [[nodiscard]] T start( const ::BS::size_t block ) const
    {
        return static_cast<T>( m_first + ( block * m_size ) / m_blocks );
    }

    [[nodiscard]] T end( const ::BS::size_t block ) const
    {
        return block == m_blocks - 1 ? m_index_after_last
                                     : static_cast<T>( m_first + ( ( block + 1 ) * m_size ) / m_blocks );
    }

    [[nodiscard]] ::BS::size_t get_num_blocks() const { return m_blocks; }

private:
    T            m_first = 0;
    T            m_index_after_last = 0;
    ::BS::size_t m_blocks = 0;
    ::BS::size_t m_size = 0;
};


template <opt_t OptFlags = tp::none>
class thread_pool
{
public:
    thread_pool() = default;

    explicit thread_pool( const ::BS::size_t ) {}

    template <typename F>
    thread_pool( const ::BS::size_t, F&& init )
    {
        // The real pool runs this once per worker thread.  There is exactly one
        // "worker" here -- the caller -- and it is already running.
        runInit( std::forward<F>( init ) );
    }

    thread_pool( const thread_pool& ) = delete;
    thread_pool& operator=( const thread_pool& ) = delete;

    ~thread_pool() = default;

    // -- submission ---------------------------------------------------------

    template <typename F, typename R = std::invoke_result_t<std::decay_t<F>>>
    [[nodiscard]] std::future<R> submit_task( F&& task, const priority_t = 0 )
    {
        std::promise<R> promise;

        try
        {
            if constexpr( std::is_void_v<R> )
            {
                task();
                promise.set_value();
            }
            else
            {
                promise.set_value( task() );
            }
        }
        catch( ... )
        {
            promise.set_exception( std::current_exception() );
        }

        return promise.get_future();
    }

    template <typename T1, typename T2, typename T = common_index_type_t<T1, T2>, typename F,
              typename R = std::invoke_result_t<std::decay_t<F>, T, T>>
    [[nodiscard]] multi_future<R> submit_blocks( const T1 first_index, const T2 index_after_last_,
                                                 F&& block, const std::size_t num_blocks = 0,
                                                 const priority_t = 0 )
    {
        multi_future<R> futures;

        const T first = static_cast<T>( first_index );
        const T index_after_last = static_cast<T>( index_after_last_ );

        if( index_after_last <= first )
            return futures;

        const index_range<T> blocks( first, index_after_last, num_blocks );
        futures.reserve( blocks.get_num_blocks() );

        for( ::BS::size_t b = 0; b < blocks.get_num_blocks(); ++b )
            futures.push_back( run<R>( block, blocks.start( b ), blocks.end( b ) ) );

        return futures;
    }

    template <typename T1, typename T2, typename T = common_index_type_t<T1, T2>, typename F>
    [[nodiscard]] multi_future<void> submit_loop( const T1 first_index, const T2 index_after_last_,
                                                  F&& loop, const std::size_t num_blocks = 0,
                                                  const priority_t = 0 )
    {
        multi_future<void> futures;

        const T first = static_cast<T>( first_index );
        const T index_after_last = static_cast<T>( index_after_last_ );

        if( index_after_last <= first )
            return futures;

        const index_range<T> blocks( first, index_after_last, num_blocks );
        futures.reserve( blocks.get_num_blocks() );

        for( ::BS::size_t b = 0; b < blocks.get_num_blocks(); ++b )
        {
            std::promise<void> promise;

            try
            {
                for( T i = blocks.start( b ); i < blocks.end( b ); ++i )
                    loop( i );

                promise.set_value();
            }
            catch( ... )
            {
                promise.set_exception( std::current_exception() );
            }

            futures.push_back( promise.get_future() );
        }

        return futures;
    }

    template <typename F>
    void detach_task( F&& task, const priority_t = 0 )
    {
        // A detached task's exception is swallowed by the real pool (it has no
        // future to store it in and must not kill the worker), so do the same.
        try
        {
            task();
        }
        catch( ... )
        {
        }
    }

    template <typename T1, typename T2, typename T = common_index_type_t<T1, T2>, typename F>
    void detach_blocks( const T1 first_index, const T2 index_after_last_, F&& block,
                        const std::size_t num_blocks = 0, const priority_t = 0 )
    {
        const T first = static_cast<T>( first_index );
        const T index_after_last = static_cast<T>( index_after_last_ );

        if( index_after_last <= first )
            return;

        const index_range<T> blocks( first, index_after_last, num_blocks );

        for( ::BS::size_t b = 0; b < blocks.get_num_blocks(); ++b )
        {
            try
            {
                block( blocks.start( b ), blocks.end( b ) );
            }
            catch( ... )
            {
            }
        }
    }

    template <typename T1, typename T2, typename T = common_index_type_t<T1, T2>, typename F>
    void detach_loop( const T1 first_index, const T2 index_after_last_, F&& loop,
                      const std::size_t num_blocks = 0, const priority_t = 0 )
    {
        const T first = static_cast<T>( first_index );
        const T index_after_last = static_cast<T>( index_after_last_ );

        if( index_after_last <= first )
            return;

        try
        {
            for( T i = first; i < index_after_last; ++i )
                loop( i );
        }
        catch( ... )
        {
        }
    }

    // -- waiting ------------------------------------------------------------
    //
    // Nothing is ever outstanding, so all of these are no-ops that report
    // "everything finished".

    void wait() {}

    template <typename R, typename P>
    bool wait_for( const std::chrono::duration<R, P>& )
    {
        return true;
    }

    template <typename C, typename D>
    bool wait_until( const std::chrono::time_point<C, D>& )
    {
        return true;
    }

    void purge() {}

    void reset() {}

    void reset( const ::BS::size_t ) {}

    // -- introspection ------------------------------------------------------

    [[nodiscard]] ::BS::size_t get_thread_count() const { return 1; }

    [[nodiscard]] ::BS::size_t get_tasks_queued() const { return 0; }

    [[nodiscard]] ::BS::size_t get_tasks_running() const { return 0; }

    [[nodiscard]] ::BS::size_t get_tasks_total() const { return 0; }

private:
    template <typename R, typename F, typename... Args>
    std::future<R> run( F&& fn, Args&&... args )
    {
        std::promise<R> promise;

        try
        {
            if constexpr( std::is_void_v<R> )
            {
                fn( std::forward<Args>( args )... );
                promise.set_value();
            }
            else
            {
                promise.set_value( fn( std::forward<Args>( args )... ) );
            }
        }
        catch( ... )
        {
            promise.set_exception( std::current_exception() );
        }

        return promise.get_future();
    }

    /// The real pool's init function may take the worker index or nothing.
    template <typename F>
    void runInit( F&& init )
    {
        if constexpr( std::is_invocable_v<F, ::BS::size_t> )
            init( ::BS::size_t( 0 ) );
        else if constexpr( std::is_invocable_v<F> )
            init();
    }
};


using light_thread_pool = thread_pool<tp::none>;
using priority_thread_pool = thread_pool<tp::priority>;
using pause_thread_pool = thread_pool<tp::pause>;
using wdc_thread_pool = thread_pool<tp::wait_deadlock_checks>;

} // namespace BS

#endif // BS_INLINE_THREAD_POOL_HPP
