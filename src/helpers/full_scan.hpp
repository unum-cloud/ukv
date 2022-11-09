/**
 * @file full_scan.hpp
 * @author Ashot Vardanian
 *
 * @brief Callback-based full-scan over BLOB collection.
 */
#pragma once
#include "ukv/blobs.h"

namespace unum::ukv {

template <typename callback_should_continue_at>
void full_scan_collection( //
    ukv_database_t db,
    ukv_transaction_t transaction,
    ukv_collection_t collection,
    ukv_options_t options,
    ukv_key_t start_key,
    ukv_length_t read_ahead,
    linked_memory_lock_t& arena,
    ukv_error_t* error,
    callback_should_continue_at&& callback_should_continue) noexcept {

    read_ahead = std::max<ukv_length_t>(read_ahead, 2u);
    while (!*error) {
        ukv_length_t* found_blobs_count = nullptr;
        ukv_key_t* found_blobs_keys = nullptr;
        ukv_scan_t scan {
            .db = db,
            .error = error,
            .transaction = transaction,
            .arena = arena,
            .options = options,
            .collections = &collection,
            .start_keys = &start_key,
            .count_limits = &read_ahead,
            .counts = &found_blobs_count,
            .keys = &found_blobs_keys,
        };

        ukv_scan(&scan);
        if (*error)
            break;

        if (found_blobs_count[0] <= 1)
            // We have reached the end of collection
            break;

        ukv_length_t* found_blobs_offsets = nullptr;
        ukv_byte_t* found_blobs_data = nullptr;
        ukv_read_t read {
            .db = db,
            .error = error,
            .transaction = transaction,
            .arena = arena,
            .options = ukv_options_t(options | ukv_option_dont_discard_memory_k),
            .tasks_count = found_blobs_count[0],
            .collections = &collection,
            .collections_stride = 0,
            .keys = found_blobs_keys,
            .keys_stride = sizeof(ukv_key_t),
            .offsets = &found_blobs_offsets,
            .values = &found_blobs_data,
        };
        ukv_read(&read);
        if (*error)
            break;

        ukv_length_t const count_blobs = found_blobs_count[0];
        joined_blobs_iterator_t found_blobs {found_blobs_offsets, found_blobs_data};
        for (std::size_t i = 0; i != count_blobs; ++i, ++found_blobs) {
            value_view_t bucket = *found_blobs;
            if (!callback_should_continue(found_blobs_keys[i], bucket))
                return;
        }

        start_key = found_blobs_keys[count_blobs - 1] + 1;
    }
}

} // namespace unum::ukv
