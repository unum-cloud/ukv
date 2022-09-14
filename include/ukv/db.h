/**
 * @file db.h
 * @author Ashot Vardanian
 * @date 12 Jun 2022
 * @brief C bindings for binary collections.
 *
 * @section Why prefer batch APIs?
 * Using the batch APIs to issue a single read/write request
 * is trivial, but achieving batch-level performance with
 * singular operations is impossible. Regardless of IO layer,
 * a lot of synchronization and locks must be issued to provide
 * consistency.
 *
 * @section Iterators
 * Implementing consistent iterators over concurrent state is exceptionally
 * expensive, thus we plan to implement those via "Pagination".
 *
 * @section Interface Conventions
 * 1. We try to expose just opaque struct pointers and functions to
 * 	  clients. This allows us to change internal representations
 *    without forcing clients to recompile code, that uses shared lib.
 * 2. Errors are encoded into NULL-terminated C strings.
 * 3. Functions that accept `collections`, @b can receive 0, 1 or N such
 *    arguments, where N is the number of passed `keys`.
 * 4. Collections, Iterators and Transactions are referencing the DB,
 *    so the DB shouldn't die/close before those objects are freed.
 *    This also allows us to reduce the number of function arguments for
 *    interface functions.
 * 5. Strides! Higher level systems may pack groups of arguments into AoS
 *    instead of SoA. To minimize the need of copies and data re-layout,
 *    we use @b byte-length strides arguments, similar to BLAS libraries.
 *    Passing Zero as a "stride" means repeating the same value.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*********************************************************/
/*****************   Structures & Consts  ****************/
/*********************************************************/

typedef void* ukv_database_t;
typedef void* ukv_transaction_t;

/**
 * @brief Some unique integer identifier of a collection.
 * A `ukv_database_t` database can have many of those, but never with
 * repeating names or identifiers.
 */
typedef uint64_t ukv_collection_t;

/**
 * @brief The unique identifier of any value.
 */
typedef int64_t ukv_key_t;

/**
 * @brief The elementary binary piece of any value.
 */
typedef uint8_t ukv_byte_t;

/**
 * @brief The elementary piece of any string, like collection name.
 */
typedef char ukv_char_t;

/**
 * @brief The length of any value in the DB.
 */
typedef uint32_t ukv_length_t;
typedef uint64_t ukv_size_t;

/**
 * @brief The smallest possible "bitset" type, storing eight zeros or ones.
 */
typedef uint8_t ukv_octet_t;

/**
 * @brief Owning error message string.
 * If not null, must be deallocated via `ukv_error_free`.
 */
typedef char const* ukv_error_t;

/**
 * @brief Non-owning string reference.
 * Always provided by user and we don't participate
 * in its lifetime management in any way.
 */
typedef char const* ukv_str_view_t;
typedef char* ukv_str_span_t;

/**
 * @brief Temporary memory handle, used mostly for read requests.
 * It's allocated, resized and deallocated only by UKV itself.
 */
typedef void* ukv_arena_t;

typedef uint8_t* ukv_bytes_ptr_t;
typedef uint8_t const* ukv_bytes_cptr_t;

typedef enum {

    ukv_options_default_k = 0,
    /**
     * @brief Forces absolute consistency on the write operations
     * flushing all the data to disk after each write. It's usage
     * may cause severe performance degradation in some implementations.
     * Yet the users must be warned, that modern IO drivers still often
     * can't guarantee that everything will reach the disk.
     */
    ukv_option_write_flush_k = 1 << 1,
    /**
     * @brief When reading from a transaction, tracks requested keys.
     * If the requested key was updated since the read, the transaction
     * will fail on commit or prior to that.
     */
    ukv_option_read_track_k = 1 << 2,
    /**
     * @brief When a transaction is started with this flag, a persistent
     * snapshot is created. It guarantees that the global state of all the
     * keys in the DB will be unchanged during the entire lifetime of the
     * transaction. Will not affect the writes in any way.
     */
    ukv_option_txn_snapshot_k = 1 << 3,
    /**
     * @brief This flag is intended for internal use.
     * When passed to `prepare_arena`, old_arena is not released,
     * and rather a new one is casted and returned,
     * if it existed in the first place, otherwise behaviour is unaffected.
     */
    ukv_option_nodiscard_k = 1 << 4,
    /**
     * @brief Will output data into shared memory, not the one privately
     * viewed by current process. That will allow any higher-level package
     * to do further transformations without any copies.
     * Is relevant for standalone distributions used with drivers supporting
     * Apache Arrow buffers or standardized Tensor representations.
     */
    ukv_option_read_shared_k = 1 << 5,
    /**
     * @brief When set, the underlying engine may avoid strict keys ordering
     * and may include irrelevant (deleted & duplicate) keys in order to maximize
     * throughput. The purpose is not accelerating the `ukv_scan`, but the
     * following `ukv_read`. Generally used for Machine Learning applications.
     */
    ukv_option_scan_bulk_k = 0, // TODO
    /**
     * @brief When set, the matching scan range will be randomly sampled
     * as close to the uniform distribution, as possible.
     */
    ukv_option_scan_sample_k = 0, // TODO

} ukv_options_t;

typedef enum {
    ukv_drop_vals_k = 0,
    ukv_drop_keys_vals_k = 1,
    ukv_drop_keys_vals_handle_k = 2,
} ukv_drop_mode_t;

extern ukv_collection_t const ukv_collection_main_k;
extern ukv_length_t const ukv_length_missing_k;
extern ukv_key_t const ukv_key_unknown_k;

extern bool const ukv_supports_transactions_k;
extern bool const ukv_supports_named_collections_k;

/*********************************************************/
/*****************	 Primary Functions	  ****************/
/*********************************************************/

/**
 * @brief Opens the underlying Key-Value Store, which can be any of:
 * > embedded persistent transactional KVS
 * > embedded in-memory transactional KVS
 * > remote persistent transactional KVS
 * > remote in-memory transactional KVS
 *
 * @param[in] config  A NULL-terminated @b JSON string with configuration specs.
 * @param[out] db     A pointer to the opened KVS, unless @param error is filled.
 * @param[out] error  The error message to be handled by callee.
 */
void ukv_database_open( //
    ukv_str_view_t config,
    ukv_database_t* db,
    ukv_error_t* error);

/**
 * @brief The primary "setter" interface.
 * Passing NULLs into @param values is identical to deleting entries.
 * If a fail had occurred, @param error will be set to non-NULL.
 *
 * @section Functionality Matrix
 * This is one of the two primary methods, that knots together various kinds of reads:
 * > Transactional and Heads
 * > Insertions and Deletions
 *
 * If lengths aren't provided, they are inferred from the passed values,
 * as the offset of the first NULL-termination (zero) symbol.
 *
 * @param[in] db             Already open database instance, @see `ukv_database_open`.
 * @param[in] txn            Transaction, through which the operation must go.
 *                           Can be NULL.
 * @param[in] tasks_count    Number of elements in @param keys.
 *
 * @param[in] collections    Array of collections owning the @param keys.
 *                           If NULL is passed, the default collection is assumed.
 *                           If multiple collections are passed, the step between
 *                           them is equal to @param collections_stride @b bytes!
 *                           Zero stride would redirect all the keys to the same collection.
 * @param[in] keys           Array of keys in one or more collections.
 *                           If multiple keys are passed, the step between
 *                           them is equal to @param keys_stride @b bytes!
 *                           Zero stride is not allowed!
 *
 * @param[in] options        Write options.
 *
 * @param[in] values         Pointer to a tape of concatenated values to be imported.
 *                           A NULL `value` means that the key mist be deleted.
 *                           To clear the `value` without removing the key, just
 *                           pass a zero length.
 *                           If multiple values are passed, the step between their
 *                           begin pointers is equal to @param values_stride @b bytes!
 *                           Zero stride would map all the keys to the same value.
 *
 * @param[in] lengths        Pointer to lengths of chunks in packed into @param values.
 * @param[in] offsets        Pointer to offsets of relevant content within @param values chunks.
 * @param[in] presences          Pointer to bitset with at least @param values bits in it.
 *
 * @param[out] error         The error to be handled.
 * @param[inout] arena       Temporary memory region, that can be reused between operations.
 *
 * @section Upserts, Updates & Inserts
 * Higher-level interfaces may choose to implement any of those verbs:
 * 1. Insert: add if missing.
 * 2. Update: overwrite if present.
 * 3. Upsert: write.
 * Instead of adding all three to C interface, we focus on better ACID transactions,
 * which can be used to implement any advanced multi-step operations (often including
 * conditionals), like Compare-And-Swap, without losing atomicity.
 *
 * @section Why use offsets?
 *
 * In the underlying layer, using offsets to adds no additional overhead,
 * but what is the point of using them, if we can immediately pass adjusted
 * pointers?
 * It serves two primary purposes:
 * > Supporting input tapes (values_stride = 0, offsets_stride != 0).
 * > List-oriented wrappers (values_stride != 0, offsets_stride = 0).
 *
 * In the first case, we may have received a tape from `ukv_read`, which we
 * update in-place and write back, without changing the size of the original
 * entries.
 *
 * In the second case, we may be working with higher-level runtimes, like
 * CPython, where objects metadata (like its length) is stored in front of
 * the allocated region. In such cases, we may still need additional memory
 * to store the lengths of the objects, unless those are NULL-terminated
 * strings (lengths = NULL) or if all have the same length (length_stride = 0).
 *
 * Further reading on the implementation of strings and arrays of strings in
 * different languages:
 * > Python/CPython:
 *      https://docs.python.org/3/c-api/bytes.html
 * > JavaScript/V8:
 *      https://github.com/v8/v8/blob/main/include/v8-data.h
 *      https://github.com/v8/v8/blob/main/include/v8-array-buffer.h
 * > GoLang:
 *      https://boakye.yiadom.org/go/strings/
 *      https://github.com/golang/go/blob/master/src/runtime/string.go (`stringStruct`)
 *      https://github.com/golang/go/blob/master/src/runtime/slice.go (`slice`)
 *
 * @section Argument Resolution Order
 * As different combinations of "value" arguments are possible, we must define the resolution order.
 * 1. If @param values is NULL or key at index `i` is NULL, we will remote the entry from DB.
 * 2. If @param presences is not NULL and bit at index `i` is set, we will remote the entry from DB.
 * 3. If @param lengths is NULL, we expect `N+1` @param offsets (like Arrow) to determine entry length.
 * 4. If @param lengths and @param offsets are NULL, we expect NULL-terminated entries and will `std::strlen`.
 */
void ukv_write( //
    ukv_database_t const db,
    ukv_transaction_t const txn,
    ukv_size_t const tasks_count,

    ukv_collection_t const* collections,
    ukv_size_t const collections_stride,

    ukv_key_t const* keys,
    ukv_size_t const keys_stride,

    ukv_octet_t const* presences,

    ukv_length_t const* offsets,
    ukv_size_t const offsets_stride,

    ukv_length_t const* lengths,
    ukv_size_t const lengths_stride,

    ukv_bytes_cptr_t const* values,
    ukv_size_t const values_stride,

    ukv_options_t const options,

    ukv_arena_t* arena,
    ukv_error_t* error);

/**
 * @brief The primary "getter" interface.
 * If a fail had occurred, @param error will be set to non-NULL.
 * Otherwise, the tape will be populated with @param tasks_count objects
 * of type `ukv_length_t`, describing the lengths of objects packed
 * right after the lengths themselves.
 * If a key wasn't found in target collection, the length will be zero.
 *
 * @section Functionality Matrix
 * This is one of the two primary methods, that knots together various kinds of reads:
 * > Transactional and Heads
 * > Single and Batch
 * > Size Estimates and Exports
 *
 * @param[in] db             Already open database instance, @see `ukv_database_open`.
 * @param[in] txn            Transaction or the snapshot, through which the
 * @param[in] tasks_count    Number of elements in @param keys.
 *
 * @param[in] collections    Array of collections owning the @param keys.
 *                           If NULL is passed, the default collection is assumed.
 *                           If multiple collections are passed, the step between
 *                           them is equal to @param collections_stride @b bytes!
 *                           Zero stride would redirect all the keys to the same collection.
 * @param[in] keys           Array of keys in one or more collections.
 *                           If multiple keys are passed, the step between
 *                           them is equal to @param keys_stride @b bytes!
 *                           Zero stride is not allowed!
 *
 * @param[in] options        Read options:
 *                           > track: Adds collision-detection on keys read through txn.
 *                           > shared: Exports to shared memory to accelerate inter-process communication.
 *                           > bulk: Suggests that the list of keys was received from a bulk scan.
 *
 * @param[out] presences     Will contain a bitset with at least @param tasks_count bits.
 *                           Each set bit means that such key is missing in DB. Is @b optional.
 *
 * @param[out] offsets       Will contain a pointer to an array with @param tasks_count integers.
 *                           Each marks a response offset in bytes starting from @param values.
 *                           To be fully compatible with Apache Arrow we append one more offset at
 *                           at the end to allow inferring the length of the last entry without
 *                           using @param lengths.
 *                           Is @b optional, as you may only want @param lengths or @param presences.
 *
 * @param[out] lengths       Will contain a pointer to an array with @param tasks_count integers.
 *                           Each defines a response length in bytes. Is @b optional.
 *
 * @param[out] values        Will contain the "base pointer" for @param tasks_count concatenated values.
 *                           Instead of allocating every "string" separately, we join them into
 *                           a single "tape" structure, which later be exported into (often disjoint)
 *                           runtime- or library-specific implementations. To determine the range of each
 *                           chunk use @param offsets and @param lengths.
 *                           Is @b optional, as you may only want @param lengths or @param presences.
 *
 *
 * @param[out] error         The error message to be handled by callee.
 * @param[inout] arena       Temporary memory region, that can be reused between operations.
 */
void ukv_read( //
    ukv_database_t const db,
    ukv_transaction_t const txn,
    ukv_size_t const tasks_count,

    ukv_collection_t const* collections,
    ukv_size_t const collections_stride,

    ukv_key_t const* keys,
    ukv_size_t const keys_stride,

    ukv_options_t const options,

    ukv_octet_t** presences,
    ukv_length_t** offsets,
    ukv_length_t** lengths,
    ukv_byte_t** values,

    ukv_arena_t* arena,
    ukv_error_t* error);

/**
 * @brief Retrieves the following (upto) @param scan_limits[i] keys starting
 * from @param start_key[i] or the smallest following key in each collection.
 * Values are not exported, for that - follow up with @see ukv_read.
 *
 * @param[in] db             Already open database instance, @see `ukv_database_open`.
 * @param[in] txn            Transaction or the snapshot, through which the
 * @param[in] tasks_count    Number of elements in @param start_keys.
 *
 * @param[in] collections    Array of collections owning the @param start_keys.
 *                           If NULL is passed, the default collection is assumed.
 *                           If multiple collections are passed, the step between
 *                           them is equal to @param collections_stride @b bytes!
 *                           Zero stride would redirect all the keys to the same collection.
 *
 * @param[in] start_keys     Array of first keys of interest in one or more collections.
 *                           If multiple keys are passed, the step between
 *                           them is equal to @param start_keys_stride @b bytes!
 *                           Zero stride is not allowed!
 *
 * @param[in] end_keys       Array of termination keys for every scan. @b Optional.
 *                           If multiple keys are passed, the step between
 *                           them is equal to @param start_keys_stride @b bytes!
 *                           Zero stride is not allowed!
 *
 * @param[in] options        Read options:
 *                           > track: Adds collision-detection on keys read through txn.
 *                           > lengths: Will fetches lengths of values, after the keys.
 *                           > bulk: Skips keys ordering and relevance checks for speed.
 *
 * @param[out] offsets       Will contain the offset of each run in joined @param keys.
 *                           Will have @param tasks_count +1 outputs, to match Arrow format.
 * @param[out] counts        Will contain the height of each column (< scan_limit).
 * @param[out] keys          Will contain columns of following keys for each task.
 *
 * @param[out] error         The error message to be handled by callee.
 * @param[inout] arena       Temporary memory region, that can be reused between operations.
 */
void ukv_scan( //
    ukv_database_t const db,
    ukv_transaction_t const txn,
    ukv_size_t const tasks_count,

    ukv_collection_t const* collections,
    ukv_size_t const collections_stride,

    ukv_key_t const* start_keys,
    ukv_size_t const start_keys_stride,

    ukv_key_t const* end_keys,
    ukv_size_t const end_keys_stride,

    ukv_length_t const* scan_limits,
    ukv_size_t const scan_limits_stride,

    ukv_options_t const options,

    ukv_length_t** offsets,
    ukv_length_t** counts,
    ukv_key_t** keys,

    ukv_arena_t* arena,
    ukv_error_t* error);

/**
 * @brief Estimates the number of entries and memory usage for a range of keys.
 *
 * @param[in] db             Already open database instance, @see `ukv_database_open`.
 * @param[in] txn            Transaction or the snapshot, through which the
 * @param[in] tasks_count    Number ranges to be introspected.
 *
 * @param[in] collections    Array of collections owning the @param keys.
 *                           If NULL is passed, the default collection is assumed.
 *                           If multiple collections are passed, the step between
 *                           them is equal to @param collections_stride @b bytes!
 *                           Zero stride would redirect all the keys to the same collection.
 * @param[in] start_keys     For every task contains the beginning of range-of-interest.
 * @param[in] end_keys       For every task contains the ending of range-of-interest.
 *
 * @param[out] estimates     For every task (range) will export @b six integers:
 *                           > min & max cardinality,
 *                           > min & max bytes in values,
 *                           > min & max (persistent) memory usage.
 *                           The memory must be allocated and provided by the user.
 *
 * @param[out] error         The error message to be handled by callee.
 * @param[inout] arena       Temporary memory region, that can be reused between operations.
 */
void ukv_size( //
    ukv_database_t const db,
    ukv_transaction_t const txn,
    ukv_size_t const tasks_count,

    ukv_collection_t const* collections,
    ukv_size_t const collections_stride,

    ukv_key_t const* start_keys,
    ukv_size_t const start_keys_stride,

    ukv_key_t const* end_keys,
    ukv_size_t const end_keys_stride,

    ukv_options_t const options,

    ukv_size_t** min_cardinalities,
    ukv_size_t** max_cardinalities,
    ukv_size_t** min_value_bytes,
    ukv_size_t** max_value_bytes,
    ukv_size_t** min_space_usages,
    ukv_size_t** max_space_usages,

    ukv_arena_t* arena,
    ukv_error_t* error);

/*********************************************************/
/***************** Collection Management  ****************/
/*********************************************************/

/**
 * @brief Inserts a new named collection into DB or opens existing one.
 * This function may never be called, as the default nameless collection
 * always exists and can be addressed via `ukv_collection_main_k`.
 *
 * @param[in] db           Already open database instance, @see `ukv_database_open`.
 * @param[in] name         A NULL-terminated collection name.
 * @param[in] config       A NULL-terminated configuration string.
 * @param[out] id          Address to which the collection ID will be exported.
 * @param[out] error       The error message to be handled by callee.
 */
void ukv_collection_open( //
    ukv_database_t const db,
    ukv_str_view_t name,
    ukv_str_view_t config,
    ukv_collection_t* id,
    ukv_error_t* error);

/**
 * @brief Retrieves a list of collection IDs & names in a NULL-delimited form.
 * The default nameless collection won't be described in any form.
 *
 * @param[in] db        Already open database instance, @see `ukv_database_open`.
 * @param[out] count    Will contain the number of found unique collections.
 * @param[out] names    A NULL-terminated output string with comma-delimited column names.
 * @param[out] error    The error message to be handled by callee.
 */
void ukv_collection_list( //
    ukv_database_t const db,
    ukv_size_t* count,
    ukv_collection_t** ids,
    ukv_length_t** offsets,
    ukv_char_t** names,
    ukv_arena_t* arena,
    ukv_error_t* error);

/**
 * @brief Removes a collection or its contents depending on @param mode.
 * The default nameless collection can't be removed, only cleared.
 *
 * @param[in] db      Already open database instance, @see `ukv_database_open`.
 * @param[in] id      If the name wasn't provided, we will match a collection by ID.
 * @param[in] name    An optional NULL-terminated collection name.
 * @param[out] error  The error message to be handled by callee.
 */
void ukv_collection_drop( //
    ukv_database_t const db,
    ukv_collection_t id,
    ukv_str_view_t name,
    ukv_drop_mode_t mode,
    ukv_error_t* error);

/**
 * @brief Performs free-form queries on the DB, that may not necessarily
 * have a stable API and a fixed format output. Generally, those requests
 * are very expensive and shouldn't be executed in most applications.
 * This is the "kitchen-sink" of UKV interface, similar to `fcntl` & `ioctl`.
 *
 * @param[in] db        Already open database instance, @see `ukv_database_open`.
 * @param[in] request   Textual representation of the command.
 * @param[out] response Output text of the request.
 * @param[out] error    The error message to be handled by callee.
 *
 * @section Available Commands
 * > "clear":   Removes all the data from DB, while keeping collection names.
 * > "reset":   Removes all the data from DB, including collection names.
 * > "compact": Flushes and compacts all the data in LSM-tree implementations.
 * > "info":    Metadata about the current software version, used for debugging.
 * > "usage":   Metadata about approximate collection sizes, RAM and disk usage.
 */
void ukv_database_control( //
    ukv_database_t const db,
    ukv_str_view_t request,
    ukv_str_view_t* response,
    ukv_error_t* error);

/*********************************************************/
/*****************		Transactions	  ****************/
/*********************************************************/

/**
 * @brief Begins a new ACID transaction or resets an existing one.
 *
 * @param db[in]            Already open database instance, @see `ukv_database_open`.
 * @param generation[in]    If equal to 0, a new number will be generated on the fly.
 * @param txn[inout]        May be pointing to an existing transaction.
 *                          In that case, it's reset to new @param generation.
 * @param error[out]        The error message to be handled by callee.
 */
void ukv_transaction_begin( //
    ukv_database_t const db,
    ukv_size_t const generation,
    ukv_options_t const options,
    ukv_transaction_t* txn,
    ukv_error_t* error);

/**
 * @brief Commits an ACID transaction.
 * Regardless of result, the content is preserved to allow further
 * logging, serialization or retries. The underlying memory can be
 * cleaned and reused by consecutive `ukv_transaction_begin` call.
 */
void ukv_transaction_commit( //
    ukv_database_t const db,
    ukv_transaction_t const txn,
    ukv_options_t const options,
    ukv_error_t* error);

/*********************************************************/
/*****************	 Memory Reclamation   ****************/
/*********************************************************/

/**
 * @brief A function to be used after `ukv_read` to
 * deallocate and return memory to the OS.
 * Passing NULLs is safe.
 */
void ukv_arena_free(ukv_database_t const db, ukv_arena_t const arena);

/**
 * @brief Deallocates memory used by transaction.
 * If snapshot was created via `ukv_option_txn_snapshot_k`,
 * it will be released.
 * Passing NULLs is safe.
 */
void ukv_transaction_free(ukv_database_t const db, ukv_transaction_t const txn);

/**
 * @brief Closes the DB and deallocates the state.
 * The database would still persist on disk.
 * Passing NULLs is safe.
 */
void ukv_database_free(ukv_database_t const db);

/**
 * @brief A function to be called after any function failure,
 * that resulted in a non-NULL `ukv_error_t`, even `ukv_database_open`.
 * That's why, unlike other `...free` methods, doesn't need `db`.
 * Passing NULLs is safe.
 */
void ukv_error_free(ukv_error_t const error);

#ifdef __cplusplus
} /* end extern "C" */
#endif
