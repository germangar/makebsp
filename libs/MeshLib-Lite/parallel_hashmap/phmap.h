#pragma once
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <utility>
#include <type_traits>

namespace phmap {

// Helper to hash pairs for std::unordered_map
struct HashPair {
    template <class T1, class T2>
    size_t operator()(const std::pair<T1, T2>& p) const {
        auto h1 = std::hash<T1>{}(p.first);
        auto h2 = std::hash<T2>{}(p.second);
        // Boost-style hash combine
        return h1 ^ (h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
    }
};

// Trait to detect std::pair
template <typename T>
struct is_pair : std::false_type {};
template <typename T, typename U>
struct is_pair<std::pair<T, U>> : std::true_type {};

template <typename T>
using default_hash = std::conditional_t<is_pair<T>::value, HashPair, std::hash<T>>;

template <typename K, typename V, typename Hash = default_hash<K>, typename Eq = std::equal_to<K>>
using flat_hash_map = std::unordered_map<K, V, Hash, Eq>;

template <typename T, typename Hash = default_hash<T>, typename Eq = std::equal_to<T>>
using flat_hash_set = std::unordered_set<T, Hash, Eq>;

template <typename K, typename V, typename Hash = default_hash<K>, typename Eq = std::equal_to<K>>
using parallel_flat_hash_map = std::unordered_map<K, V, Hash, Eq>;

template <typename T, typename Hash = default_hash<T>, typename Eq = std::equal_to<T>>
using parallel_flat_hash_set = std::unordered_set<T, Hash, Eq>;

namespace priv {
template <typename T>
using hash_default_hash = default_hash<T>;
template <typename T>
using hash_default_eq = std::equal_to<T>;
}

} // namespace phmap
