#pragma once

#include "MRMeshFwd.h"
#include "MRMacros.h"
#include <vector>
#include <memory>

namespace MR
{

/// \addtogroup BasicGroup
/// \{

/// returns the amount of memory given vector occupies on heap
template<typename T>
[[nodiscard]] inline size_t heapBytes( const std::vector<T> & vec )
{
    constexpr bool hasHeapBytes = requires( const T& t ) { t.heapBytes(); };
    if constexpr ( hasHeapBytes )
    {
        size_t res = vec.capacity() * sizeof( T );
        for ( const T & t : vec )
            res += t.heapBytes();
        return res;
    }
    else
    {
        return vec.capacity() * sizeof( T );
    }
}

template<typename T, typename U>
[[nodiscard]] inline size_t heapBytes( const Vector<T, U>& vec )
{
    constexpr bool hasHeapBytes = requires( const T & t ) { t.heapBytes(); };
    if constexpr ( hasHeapBytes )
    {
        size_t res = vec.size() * sizeof( T );
        for ( const T & t : vec )
            res += t.heapBytes();
        return res;
    }
    else
    {
        return vec.size() * sizeof( T );
    }
}

/// returns the amount of memory this smart pointer and its pointed object own together on heap
template<typename T>
[[nodiscard]] inline size_t heapBytes( const std::unique_ptr<T> & ptr )
{
    if ( !ptr )
        return 0;
    return sizeof( T ) + ptr->heapBytes();
}

/// returns the amount of memory this smart pointer and its pointed object own together on heap
template<typename T>
[[nodiscard]] inline size_t heapBytes( const std::shared_ptr<T> & ptr )
{
    if ( !ptr )
        return 0;
    return sizeof( T ) + ptr->heapBytes();
}

/// Needed for generic code, always returns zero.
/// The constraint is needed to avoid hard errors in C bindings when using MSVC STL when calling `heapBytes<SomeNonfuncType>(...)`.
template<typename T> MR_REQUIRES_IF_SUPPORTED( std::is_function_v<T> )
[[nodiscard]] inline size_t heapBytes( const std::function<T> & )
{
    return 0;
}

template<typename K, typename V, typename Hash, typename Eq>
[[nodiscard]] inline size_t heapBytes( const phmap::flat_hash_map<K, V, Hash, Eq>& )
{
    return 0;
}

template<typename T, typename Hash, typename Eq>
[[nodiscard]] inline size_t heapBytes( const phmap::flat_hash_set<T, Hash, Eq>& )
{
    return 0;
}

/// \}

} // namespace MR
