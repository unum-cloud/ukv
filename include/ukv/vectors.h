/**
 * @file paths.h
 * @author Ashot Vardanian
 * @date 23 Sep 2022
 * @addtogroup C
 *
 * @brief Binary Interface Standard for @b Vector collections.
 */

#pragma once

#include "db.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {

    ukv_vector_metric_cos_k = 0,
    ukv_vector_metric_dot_k = 1,
    ukv_vector_metric_l2_k = 2,

} ukv_vector_metric_t;

typedef enum {

    ukv_vector_scalar_f32_k = 0,
    ukv_vector_scalar_f16_k = 1,
    ukv_vector_scalar_i8_k = 2,
    ukv_vector_scalar_f64_k = 3,

} ukv_vector_scalar_t;

/**
 * @brief Maps keys to High-Dimensional Vectors.
 * Generalization of @c ukv_write_t to numerical vectors.
 * @see `ukv_vectors_write()`, `ukv_write_t`, `ukv_write()`.
 *
 * Assuming all the vectors within the operation will have the
 * same dimensionality and their scalar components would form
 * continuous chunks, we need less arguments for this call,
 * than some binary methods.
 */
typedef struct ukv_vectors_write_t {

    /// @name Context
    /// @{

    /** @brief Already open database instance. */
    ukv_database_t db;
    /** @brief Pointer to exported error message. */
    ukv_error_t* error;
    /** @brief The transaction in which the operation will be watched. */
    ukv_transaction_t transaction = NULL;
    /** @brief Reusable memory handle. */
    ukv_arena_t* arena = NULL;
    /** @brief Read and Write options for Read-Modify-Write logic. @see `ukv_read_t`, `ukv_write_t`. */
    ukv_options_t options = ukv_options_default_k;

    /// @}
    /// @name Inputs
    /// @{

    ukv_size_t tasks_count = 1;
    ukv_length_t dimensions;
    ukv_vector_scalar_t scalar_type = ukv_vector_scalar_f32_k;

    ukv_collection_t const* collections = NULL;
    ukv_size_t collections_stride = 0;

    ukv_key_t const* keys;
    ukv_size_t keys_stride = 0;

    ukv_bytes_cptr_t const* vectors_starts;
    ukv_size_t vectors_starts_stride = 0;
    ukv_size_t vectors_stride = 0;

    ukv_length_t const* offsets = NULL;
    ukv_size_t offsets_stride = 0;

    // @}

} ukv_vectors_write_t;

/**
 * @brief Maps keys to High-Dimensional Vectors.
 * Generalization of @c ukv_write_t to numerical vectors.
 * @see `ukv_vectors_write_t`, `ukv_write_t`, `ukv_write()`.
 */
void ukv_vectors_write(ukv_vectors_write_t*);

/**
 * @brief Retrieves binary representations of vectors.
 * Generalization of @c ukv_read_t to numerical vectors.
 * Packs everything into a @b row-major dense matrix.
 * @see `ukv_vectors_read()`, `ukv_read_t`, `ukv_read()`.
 */
typedef struct ukv_vectors_read_t {

    /// @name Context
    /// @{

    /** @brief Already open database instance. */
    ukv_database_t db;
    /** @brief Pointer to exported error message. */
    ukv_error_t* error;
    /** @brief The transaction in which the operation will be watched. */
    ukv_transaction_t transaction = NULL;
    /** @brief Reusable memory handle. */
    ukv_arena_t* arena = NULL;
    /** @brief Read options. @see `ukv_read_t`. */
    ukv_options_t options = ukv_options_default_k;

    /// @}
    /// @name Inputs
    /// @{

    ukv_size_t tasks_count = 1;
    ukv_length_t dimensions;
    ukv_vector_scalar_t scalar_type = ukv_vector_scalar_f32_k;

    ukv_collection_t const* collections = NULL;
    ukv_size_t collections_stride = 0;

    ukv_key_t const* keys = NULL;
    ukv_size_t keys_stride = 0;

    /// @}

    /// @}
    /// @name Outputs
    /// @{
    ukv_octet_t** presences = NULL;
    ukv_length_t** offsets = NULL;
    ukv_byte_t** vectors = NULL;
    /// @}

} ukv_vectors_read_t;

/**
 * @brief Retrieves binary values given string paths.
 * Generalization of @c ukv_read_t to numerical vectors.
 * @see `ukv_vectors_read_t`, `ukv_read_t`, `ukv_read()`.
 */
void ukv_vectors_read(ukv_vectors_read_t*);

/**
 * @brief Performs K-Approximate Nearest Neighbors Search.
 * @see `ukv_vectors_search()`.
 */
typedef struct ukv_vectors_search_t {

    /// @name Context
    /// @{

    /** @brief Already open database instance. */
    ukv_database_t db;
    /** @brief Pointer to exported error message. */
    ukv_error_t* error;
    /** @brief The transaction in which the operation will be watched. */
    ukv_transaction_t transaction = NULL;
    /** @brief Reusable memory handle. */
    ukv_arena_t* arena = NULL;
    /** @brief Read options. @see `ukv_read_t`. */
    ukv_options_t options = ukv_options_default_k;

    /// @}
    /// @name Inputs
    /// @{

    ukv_size_t tasks_count = 1;
    ukv_length_t dimensions;
    ukv_vector_scalar_t scalar_type = ukv_vector_scalar_f32_k;
    ukv_vector_metric_t metric = ukv_vector_metric_cos_k;
    ukv_float_t metric_threshold = 0;

    ukv_collection_t const* collections = NULL;
    ukv_size_t collections_stride = 0;

    ukv_length_t const* match_counts_limits;
    ukv_size_t match_counts_limits_stride = 0;

    ukv_bytes_cptr_t const* queries_starts;
    ukv_size_t queries_starts_stride = 0;
    ukv_size_t queries_stride = 0;

    ukv_length_t const* queries_offsets = NULL;
    ukv_size_t queries_offsets_stride = 0;

    /// @}
    /// @name Outputs
    /// @{
    ukv_length_t** match_counts = NULL;
    ukv_length_t** match_offsets = NULL;
    ukv_key_t** match_keys = NULL;
    ukv_float_t** match_metrics = NULL;
    /// @}

} ukv_vectors_search_t;

/**
 * @brief Performs K-Approximate Nearest Neighbors Search.
 * @see `ukv_vectors_search_t`.
 */
void ukv_vectors_search(ukv_vectors_search_t*);

#ifdef __cplusplus
} /* end extern "C" */
#endif