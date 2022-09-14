/**
 * @file utility.hpp
 * @author Ashot Vardanian
 * @date 4 Jul 2022
 *
 * @brief Smart Pointers, Monads and Range-like abstractions for C++ bindings.
 */

#pragma once
#include <type_traits> // `std::void_t`

#include "ukv/cpp/types.hpp"
#include "ukv/cpp/ranges_args.hpp"

namespace unum::ukv {

// clang-format off
template <typename at, typename = int> struct sfinae_is_range_gt : std::false_type {};
template <typename at> struct sfinae_is_range_gt<at, decltype(std::data(at{}), 0)> : std::true_type {};
template <typename at> struct sfinae_is_range_gt<strided_range_gt<at>> : std::true_type {};
static_assert(!sfinae_is_range_gt<int>::value);

template <typename at, typename = int> struct sfinae_has_collection_gt : std::false_type {};
template <typename at> struct sfinae_has_collection_gt<at, decltype((void)at::collection, 0)> : std::true_type {};

template <typename at, typename = int> struct sfinae_has_field_gt : std::false_type {};
template <typename at> struct sfinae_has_field_gt<at, decltype((void)at::field, 0)> : std::true_type {};

template <typename at, typename = void> struct sfinae_member_extractor_gt { using value_type = at; };
template <typename at> struct sfinae_member_extractor_gt<at, std::void_t<typename at::value_type>> { using value_type = typename at::value_type; };
// clang-format on

template <typename locations_at>
constexpr bool is_one() {
    using locations_t = std::remove_reference_t<locations_at>;
    using element_t = typename sfinae_member_extractor_gt<locations_t>::value_type;
    return std::is_same_v<element_t, locations_t>;
}

template <typename at>
struct location_store_gt {
    static_assert(!std::is_reference_v<at>);
    using plain_t = at;
    using store_t = at;
    store_t store;

    location_store_gt() = default;
    location_store_gt(location_store_gt&&) = default;
    location_store_gt(location_store_gt const&) = default;
    location_store_gt(plain_t location) : store(std::move(location)) {}

    plain_t& ref() noexcept { return store; }
    plain_t const& ref() const noexcept { return store; }
};

template <>
struct location_store_gt<int> : public location_store_gt<collection_key_field_t> {};
template <>
struct location_store_gt<ukv_key_t> : public location_store_gt<collection_key_field_t> {};
template <>
struct location_store_gt<collection_key_t> : public location_store_gt<collection_key_field_t> {};

template <>
struct location_store_gt<int&> : public location_store_gt<collection_key_field_t> {};
template <>
struct location_store_gt<ukv_key_t&> : public location_store_gt<collection_key_field_t> {};
template <>
struct location_store_gt<collection_key_t&> : public location_store_gt<collection_key_field_t> {};

template <typename at>
struct location_store_gt<at&> {
    using store_t = at*;
    using plain_t = at;
    store_t store = nullptr;

    location_store_gt() = default;
    location_store_gt(location_store_gt&&) = default;
    location_store_gt(location_store_gt const&) = default;
    location_store_gt(plain_t& location) noexcept : store(&location) {}
    plain_t& ref() noexcept { return *store; }
    plain_t const& ref() const noexcept { return *store; }
};

template <typename locations_without_collections_at>
struct locations_in_collection_gt {
    location_store_gt<locations_without_collections_at> without;
    ukv_collection_t collection;
};

template <>
struct locations_in_collection_gt<int> : public collection_key_field_t {};
template <>
struct locations_in_collection_gt<ukv_key_t> : public collection_key_field_t {};

template <typename at, typename = void>
struct places_arg_extractor_gt {
    static_assert(!std::is_reference_v<at>);

    using location_t = at;
    using element_t = typename sfinae_member_extractor_gt<location_t>::value_type;
    static constexpr bool is_one_k = is_one<location_t>();
    static constexpr bool element_stores_collection_k = sfinae_has_collection_gt<element_t>::value;
    static constexpr bool element_stores_field_k = sfinae_has_field_gt<element_t>::value;

    ukv_size_t count(location_t const& arg) noexcept {
        if constexpr (is_one_k)
            return 1;
        else
            return std::size(arg);
    }

    strided_iterator_gt<ukv_key_t const> keys(location_t const& arg) noexcept {
        element_t const* begin = nullptr;
        if constexpr (is_one_k)
            begin = &arg;
        else
            begin = std::data(arg);

        ukv_size_t stride = 0;
        if constexpr (std::is_same_v<location_t, strided_range_gt<element_t>>)
            stride = arg.stride();
        else if constexpr (!is_one_k)
            stride = sizeof(element_t);

        auto strided = strided_iterator_gt<element_t const>(begin, stride);
        if constexpr (std::is_same_v<element_t, ukv_key_t>)
            return strided;
        else
            return strided.members(&element_t::key);
    }

    strided_iterator_gt<ukv_collection_t const> collections(location_t const& arg) noexcept {
        if constexpr (element_stores_collection_k) {
            element_t const* begin = nullptr;
            if constexpr (is_one_k)
                begin = &arg;
            else
                begin = std::data(arg);

            ukv_size_t stride = 0;
            if constexpr (std::is_same_v<location_t, strided_range_gt<element_t>>)
                stride = arg.stride();
            else if constexpr (!is_one_k)
                stride = sizeof(element_t);

            auto strided = strided_iterator_gt<element_t const>(begin, stride);
            return strided.members(&element_t::collection);
        }
        else
            return {};
    }

    strided_iterator_gt<ukv_str_view_t const> fields(location_t const& arg) noexcept {
        if constexpr (element_stores_field_k) {
            element_t const* begin = nullptr;
            if constexpr (is_one_k)
                begin = &arg;
            else
                begin = std::data(arg);

            ukv_size_t stride = 0;
            if constexpr (std::is_same_v<location_t, strided_range_gt<element_t>>)
                stride = arg.stride();
            else if constexpr (!is_one_k)
                stride = sizeof(element_t);

            auto strided = strided_iterator_gt<element_t const>(begin, stride);
            return strided.members(&element_t::field);
        }
        else
            return {};
    }
};

template <>
struct places_arg_extractor_gt<places_arg_t> {

    using location_t = places_arg_t;
    static constexpr bool is_one_k = false;

    ukv_size_t count(places_arg_t const& native) noexcept { return native.count; }
    strided_iterator_gt<ukv_key_t const> keys(places_arg_t const& native) noexcept { return native.keys_begin; }
    strided_iterator_gt<ukv_collection_t const> collections(places_arg_t const& native) noexcept {
        return native.collections_begin;
    }
    strided_iterator_gt<ukv_str_view_t const> fields(places_arg_t const& native) noexcept {
        return native.fields_begin;
    }
};

template <typename at>
struct places_arg_extractor_gt<locations_in_collection_gt<at>> {

    using location_t = locations_in_collection_gt<at>;
    using base_t = places_arg_extractor_gt<at>;
    static constexpr bool is_one_k = places_arg_extractor_gt<at>::is_one_k;

    ukv_size_t count(location_t const& arg) noexcept { return base_t {}.count(arg.without.ref()); }
    strided_iterator_gt<ukv_key_t const> keys(location_t const& arg) noexcept {
        return base_t {}.keys(arg.without.ref());
    }
    strided_iterator_gt<ukv_collection_t const> collections(location_t const& arg) noexcept {
        return {&arg.collection};
    }
    strided_iterator_gt<ukv_str_view_t const> fields(location_t const& arg) noexcept {
        return base_t {}.fields(arg.without.ref());
    }
};

template <typename at>
struct places_arg_extractor_gt<at&> : public places_arg_extractor_gt<at> {};

template <typename at, typename = void>
struct contents_arg_extractor_gt {};

template <>
struct contents_arg_extractor_gt<char const**> {
    strided_iterator_gt<ukv_bytes_cptr_t const> contents(char const** strings) noexcept {
        return {(ukv_bytes_cptr_t const*)strings, sizeof(char const*)};
    }
    strided_iterator_gt<ukv_length_t const> offsets(char const**) noexcept { return {}; }
    strided_iterator_gt<ukv_length_t const> lengths(char const**) noexcept { return {}; }
};

template <>
struct contents_arg_extractor_gt<char const*> {
    strided_iterator_gt<ukv_bytes_cptr_t const> contents(char const*& one) noexcept {
        return {(ukv_bytes_cptr_t const*)&one, 0};
    }
    strided_iterator_gt<ukv_length_t const> offsets(char const*) noexcept { return {}; }
    strided_iterator_gt<ukv_length_t const> lengths(char const*) noexcept { return {}; }
};

template <size_t length_ak>
struct contents_arg_extractor_gt<char const [length_ak]> {
    strided_iterator_gt<ukv_bytes_cptr_t const> contents(char const*& one) noexcept {
        return {(ukv_bytes_cptr_t const*)&one, 0};
    }
    strided_iterator_gt<ukv_length_t const> offsets(char const*) noexcept {
        return {};
    }
    strided_iterator_gt<ukv_length_t const> lengths(char const*) noexcept {
        return {};
    }
};

template <size_t length_ak>
struct contents_arg_extractor_gt<value_view_t[length_ak]> {
    strided_iterator_gt<ukv_bytes_cptr_t const> contents(value_view_t* many) noexcept {
        return {many->member_ptr(), sizeof(value_view_t)};
    }
    strided_iterator_gt<ukv_length_t const> offsets(value_view_t*) noexcept { return {}; }
    strided_iterator_gt<ukv_length_t const> lengths(value_view_t* many) noexcept {
        return {many->member_length(), sizeof(value_view_t)};
    }
};

template <>
struct contents_arg_extractor_gt<value_view_t> {
    strided_iterator_gt<ukv_bytes_cptr_t const> contents(value_view_t& one) noexcept { return {one.member_ptr(), 0}; }
    strided_iterator_gt<ukv_length_t const> offsets(value_view_t) noexcept { return {}; }
    strided_iterator_gt<ukv_length_t const> lengths(value_view_t& one) noexcept { return {one.member_length(), 0}; }
};

template <>
struct contents_arg_extractor_gt<contents_arg_t> {
    strided_iterator_gt<ukv_bytes_cptr_t const> contents(contents_arg_t native) noexcept {
        return native.contents_begin;
    }
    strided_iterator_gt<ukv_length_t const> offsets(contents_arg_t native) noexcept { return native.offsets_begin; }
    strided_iterator_gt<ukv_length_t const> lengths(contents_arg_t native) noexcept { return native.lengths_begin; }
};

template <>
struct contents_arg_extractor_gt<std::nullptr_t> {
    strided_iterator_gt<ukv_bytes_cptr_t const> contents(std::nullptr_t) noexcept { return {}; }
    strided_iterator_gt<ukv_length_t const> offsets(std::nullptr_t) noexcept { return {}; }
    strided_iterator_gt<ukv_length_t const> lengths(std::nullptr_t) noexcept { return {}; }
};

template <typename allocator_at>
struct contents_arg_extractor_gt<std::vector<value_view_t, allocator_at>> {
    strided_iterator_gt<ukv_bytes_cptr_t const> contents(std::vector<value_view_t, allocator_at> const& many) noexcept {
        return many.empty()
                   ? strided_iterator_gt<ukv_bytes_cptr_t const> {}
                   : strided_iterator_gt<ukv_bytes_cptr_t const> {many.front().member_ptr(), sizeof(value_view_t)};
    }
    strided_iterator_gt<ukv_length_t const> offsets(std::vector<value_view_t, allocator_at> const&) noexcept {
        return {};
    }
    strided_iterator_gt<ukv_length_t const> lengths(std::vector<value_view_t, allocator_at> const& many) noexcept {
        return many.empty()
                   ? strided_iterator_gt<ukv_length_t const> {}
                   : strided_iterator_gt<ukv_length_t const> {many.front().member_length(), sizeof(value_view_t)};
    }
};

} // namespace unum::ukv
