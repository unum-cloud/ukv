/**
 * @file blobs_collection.hpp
 * @author Ashot Vardanian
 * @date 26 Jun 2022
 * @addtogroup Cpp
 *
 * @brief C++ bindings for "ukv/db.h".
 */

#pragma once
#include "ukv/cpp/blobs_ref.hpp"
#include "ukv/cpp/blobs_range.hpp"

namespace unum::ukv {

/**
 * @brief Collection is persistent associative container,
 * essentially a transactional @b map<id,string>.
 * Or in Python terms: @b dict[int,bytes].
 *
 * Generally cheap to construct. Can address @b both collections
 * "HEAD" state, as well as some "snapshot"/"transaction" view.
 *
 * ## Class Specs
 *
 * - Concurrency: Thread-safe, for @b unique arenas.
 *   For details, see @c blobs_ref_gt docs on "Memory Management"
 * - Lifetime: @b Must live shorter then the DB it belongs to.
 * - Exceptions: Only the `size` method.
 * - Copyable: Will create a new empty arena.
 *   Will remain attached to same transaction context, if any was set.
 *
 * ## Formats
 *
 * Formats @b loosely describe the data stored in the collection
 * and @b exactly define the communication through this exact handle.
 * Example: Same collection can accept similar formats, such
 * as `::ukv_doc_field_json_k` and `::ukv_doc_field_msgpack_k`. Both will be
 * converted into some internal hierarchical representation
 * in "Document Collections", and can later be queried with
 * any "Document Format".
 */
class blobs_collection_t {
    ukv_database_t db_ = nullptr;
    ukv_collection_t collection_ = ukv_collection_main_k;
    ukv_transaction_t txn_ = nullptr;
    any_arena_t arena_;

  public:
    inline blobs_collection_t() noexcept : arena_(nullptr) {}
    inline blobs_collection_t(ukv_database_t db_ptr,
                              ukv_collection_t collection = ukv_collection_main_k,
                              ukv_transaction_t txn = nullptr,
                              ukv_arena_t* arena = nullptr) noexcept
        : db_(db_ptr), collection_(collection), txn_(txn), arena_(db_, arena) {}

    inline blobs_collection_t(blobs_collection_t&& other) noexcept
        : db_(other.db_), collection_(std::exchange(other.collection_, ukv_collection_main_k)),
          txn_(std::exchange(other.txn_, nullptr)), arena_(std::exchange(other.arena_, {nullptr})) {}

    inline blobs_collection_t& operator=(blobs_collection_t&& other) noexcept {
        std::swap(db_, other.db_);
        std::swap(collection_, other.collection_);
        std::swap(txn_, other.txn_);
        std::swap(arena_, other.arena_);
        return *this;
    }

    inline blobs_collection_t(blobs_collection_t const& other) noexcept
        : db_(other.db_), collection_(other.collection_), txn_(other.txn_), arena_(other.db_) {}

    inline blobs_collection_t& operator=(blobs_collection_t const& other) noexcept {
        db_ = other.db_;
        collection_ = other.collection_;
        txn_ = other.txn_;
        arena_ = any_arena_t(other.db_);
        return *this;
    }

    inline operator ukv_collection_t() const noexcept { return collection_; }
    inline ukv_collection_t* member_ptr() noexcept { return &collection_; }
    inline ukv_arena_t* member_arena() noexcept { return arena_.member_ptr(); }
    inline ukv_database_t db() const noexcept { return db_; }
    inline ukv_transaction_t txn() const noexcept { return txn_; }

    inline blobs_range_t members( //
        ukv_key_t min_key = std::numeric_limits<ukv_key_t>::min(),
        ukv_key_t max_key = std::numeric_limits<ukv_key_t>::max()) const noexcept {
        return {db_, txn_, collection_, min_key, max_key};
    }
    inline keys_range_t keys( //
        ukv_key_t min_key = std::numeric_limits<ukv_key_t>::min(),
        ukv_key_t max_key = std::numeric_limits<ukv_key_t>::max()) const noexcept {
        return {members(min_key, max_key)};
    }

    inline pairs_range_t items( //
        ukv_key_t min_key = std::numeric_limits<ukv_key_t>::min(),
        ukv_key_t max_key = std::numeric_limits<ukv_key_t>::max()) const noexcept {
        return {blobs_range_t {db_, txn_, collection_, min_key, max_key}};
    }

    inline expected_gt<size_range_t> size_range() const noexcept {
        auto maybe = members().size_estimates();
        return {maybe.release_status(), std::move(maybe->cardinality)};
    }

    std::size_t size() const noexcept(false) {
        auto maybe = size_range();
        maybe.throw_unhandled();
        return (maybe->min + maybe->max) / 2;
    }

    status_t clear_values() noexcept {
        status_t status;
        ukv_collection_drop_t collection_drop {
            .db = db_,
            .error = status.member_ptr(),
            .id = collection_,
            .mode = ukv_drop_vals_k,
        };
        ukv_collection_drop(&collection_drop);
        return status;
    }

    status_t clear() noexcept {
        status_t status;
        ukv_collection_drop_t collection_drop {
            .db = db_,
            .error = status.member_ptr(),
            .id = collection_,
            .mode = ukv_drop_keys_vals_k,
        };
        ukv_collection_drop(&collection_drop);
        return status;
    }

    status_t drop() noexcept {
        status_t status;
        ukv_collection_drop_t collection_drop {
            .db = db_,
            .error = status.member_ptr(),
            .id = collection_,
            .mode = ukv_drop_keys_vals_handle_k,
        };
        ukv_collection_drop(&collection_drop);
        return status;
    }

    inline blobs_ref_gt<places_arg_t> operator[](std::initializer_list<ukv_key_t> keys) noexcept { return at(keys); }
    inline blobs_ref_gt<places_arg_t> at(std::initializer_list<ukv_key_t> keys) noexcept { //
        return at(strided_range(keys));
    }

    inline blobs_ref_gt<places_arg_t> operator[](keys_view_t keys) noexcept { return at(keys); }
    inline blobs_ref_gt<places_arg_t> at(keys_view_t keys) noexcept {
        places_arg_t arg;
        arg.collections_begin = &collection_;
        arg.keys_begin = keys.begin();
        arg.count = keys.size();
        return {db_, txn_, {std::move(arg)}, arena_};
    }

    template <typename keys_arg_at>
    auto operator[](keys_arg_at&& keys) noexcept { //
        return at(std::forward<keys_arg_at>(keys));
    }

    template <typename keys_arg_at>
    auto at(keys_arg_at&& keys) noexcept { //
        constexpr bool is_one_k = is_one<keys_arg_at>();
        if constexpr (is_one_k) {
            using result_t = blobs_ref_gt<collection_key_field_t>;
            using plain_t = std::remove_reference_t<keys_arg_at>;
            // ? We may want to warn the users, that the collection property will be shadowed:
            // static_assert(!sfinae_has_collection_gt<plain_t>::value, "Overwriting existing collection!");
            collection_key_field_t arg;
            arg.collection = collection_;
            if constexpr (std::is_integral_v<plain_t>)
                arg.key = keys;
            else
                arg.key = keys.key;

            if constexpr (sfinae_has_field_gt<plain_t>::value)
                arg.field = keys.field;
            return result_t {db_, txn_, arg, arena_};
        }
        else {
            using locations_t = locations_in_collection_gt<keys_arg_at>;
            using result_t = blobs_ref_gt<locations_t>;
            return result_t {db_, txn_, locations_t {std::forward<keys_arg_at>(keys), collection_}, arena_};
        }
    }
};

} // namespace unum::ukv
