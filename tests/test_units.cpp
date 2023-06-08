/**
 * @file test_units.cpp
 * @author Ashot Vardanian
 * @date 2022-07-06
 *
 * @brief A set of unit tests implemented using Google Test.
 */

#include <vector>
#include <unordered_set>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <csignal>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <bson.h>

#include <fmt/format.h>

#include <ustore/arrow.h>
#include "ustore/ustore.hpp"

using namespace unum::ustore;
using namespace unum;

template <typename container_at>
char const* str_begin(container_at const& container) {
    if constexpr (std::is_same_v<char const*, container_at>)
        return container;
    else if constexpr (std::is_same_v<value_view_t, container_at>)
        return reinterpret_cast<char const*>(container.begin());
    else
        return reinterpret_cast<char const*>(std::data(container));
}

template <typename container_at>
char const* str_end(container_at const& container) {
    if constexpr (std::is_same_v<char const*, container_at>)
        return container + std::strlen(container);
    else if constexpr (std::is_same_v<value_view_t, container_at>)
        return reinterpret_cast<char const*>(container.end());
    else
        return str_begin(container) + std::size(container);
}

using json_t = nlohmann::json;

static json_t json_parse(char const* begin, char const* end) {
    json_t result;
    auto adapter = nlohmann::detail::input_adapter(begin, end);
    auto parser = nlohmann::detail::parser<json_t, decltype(adapter)>(std::move(adapter), nullptr, true, true);
    parser.parse(false, result);
    return result;
    // return json_t::parse(begin, end, nullptr, true, true);
}

#define M_EXPECT_EQ_JSON(str1, str2) \
    EXPECT_EQ(json_parse(str_begin(str1), str_end(str1)), json_parse(str_begin(str2), str_end(str2)));
#define M_EXPECT_EQ_MSG(str1, str2) \
    EXPECT_EQ(json_t::from_msgpack(str_begin(str1), str_end(str1)), json_parse(str_begin(str2), str_end(str2)));

static char const* path() {
    char* path = std::getenv("USTORE_TEST_PATH");
    if (path)
        return std::strlen(path) ? path : nullptr;

#if defined(USTORE_FLIGHT_CLIENT)
    return nullptr;
#elif defined(USTORE_TEST_PATH)
    return USTORE_TEST_PATH;
#else
    return nullptr;
#endif
}

static std::string config() {
    auto dir = path();
    if (!dir)
        return {};
    return fmt::format(R"({{"version": "1.0", "directory": "{}"}})", dir);
}

#if defined(USTORE_FLIGHT_CLIENT)
static pid_t srv_id = -1;
static std::string srv_path;
#endif

void clear_environment() {
#if defined(USTORE_FLIGHT_CLIENT)
    if (srv_id > 0) {
        kill(srv_id, SIGKILL);
        waitpid(srv_id, nullptr, 0);
    }

    srv_id = fork();
    if (srv_id == 0) {
        usleep(1); // TODO Any statement is required to be run for successful `execl` run...
        execl(srv_path.c_str(), srv_path.c_str(), "--quiet", (char*)(NULL));
        exit(0);
    }
    usleep(100000); // 0.1 sec
#endif

    namespace stdfs = std::filesystem;
    auto directory_str = path() ? std::string_view(path()) : "";
    if (!directory_str.empty()) {
        stdfs::remove_all(directory_str);
        stdfs::create_directories(stdfs::path(directory_str));
    }
}

inline std::ostream& operator<<(std::ostream& os, collection_key_t obj) {
    return os << obj.collection << obj.key;
}

#pragma region Binary Modality

template <typename locations_at>
void check_length(blobs_ref_gt<locations_at>& ref, ustore_length_t expected_length) {

    EXPECT_TRUE(ref.value()) << "Failed to fetch missing keys";

    auto const expects_missing = expected_length == ustore_length_missing_k;
    using extractor_t = places_arg_extractor_gt<locations_at>;
    ustore_size_t count = extractor_t {}.count(ref.locations());

    // Validate that values match
    auto maybe_retrieved = ref.value();
    auto const& retrieved = *maybe_retrieved;
    EXPECT_EQ(retrieved.size(), count);

    // Check views
    auto it = retrieved.begin();
    for (std::size_t i = 0; i != count; ++i, ++it) {
        EXPECT_EQ((*it).size(), expects_missing ? 0 : expected_length);
    }

    // Check length estimates
    auto maybe_lengths = ref.length();
    EXPECT_TRUE(maybe_lengths);
    for (std::size_t i = 0; i != count; ++i) {
        EXPECT_EQ(maybe_lengths->at(i), expected_length);
    }

    // Check boolean indicators
    auto maybe_indicators = ref.present();
    EXPECT_TRUE(maybe_indicators);
    for (std::size_t i = 0; i != count; ++i) {
        EXPECT_EQ(maybe_indicators->at(i), !expects_missing);
    }
}

template <template <typename locations_at> class ref_at, typename locations_at>
void check_equalities(ref_at<locations_at>& ref, contents_arg_t values) {

    EXPECT_TRUE(ref.value()) << "Failed to fetch present keys";
    using extractor_t = places_arg_extractor_gt<locations_at>;

    // Validate that values match
    auto maybe_retrieved = ref.value();
    auto const& retrieved = *maybe_retrieved;
    EXPECT_EQ(retrieved.size(), extractor_t {}.count(ref.locations()));

    auto it = retrieved.begin();
    for (std::size_t i = 0; i != extractor_t {}.count(ref.locations()); ++i, ++it) {
        auto expected_len = values[i].size();
        auto expected_begin = values[i].begin();

        value_view_t retrieved_view = *it;
        value_view_t expected_view(expected_begin, expected_begin + expected_len);
        EXPECT_EQ(retrieved_view.size(), expected_view.size());
        EXPECT_EQ(retrieved_view, expected_view);
    }
}

template <typename locations_at>
void round_trip(blobs_ref_gt<locations_at>& ref, contents_arg_t values) {
    EXPECT_TRUE(ref.assign(values)) << "Failed to assign";
    check_equalities(ref, values);
}

struct triplet_t {
    static constexpr std::size_t val_size_k = sizeof(char);
    std::array<ustore_key_t, 3> keys {'a', 'b', 'c'};

    std::array<char, 3> vals {'A', 'B', 'C'};
    std::array<ustore_length_t, 3> lengths {1, 1, 1};
    std::array<ustore_length_t, 4> offsets {0, 1, 2, 3};
    ustore_octet_t presences = 1 | (1 << 1) | (1 << 2);
    std::array<ustore_bytes_ptr_t, 3> vals_pointers;

    triplet_t() noexcept {
        vals_pointers[0] = (ustore_bytes_ptr_t)&vals[0];
        vals_pointers[1] = (ustore_bytes_ptr_t)&vals[1];
        vals_pointers[2] = (ustore_bytes_ptr_t)&vals[2];
    }
    contents_arg_t contents() const noexcept { return contents_arrow(); }
    contents_arg_t contents_lengths() const noexcept {
        contents_arg_t arg {};
        arg.lengths_begin = {&lengths[0], sizeof(lengths[0])};
        arg.contents_begin = {&vals_pointers[0], sizeof(vals_pointers[0])};
        arg.count = 3;
        return arg;
    }
    contents_arg_t contents_arrow() const noexcept {
        contents_arg_t arg {};
        arg.offsets_begin = {&offsets[0], sizeof(offsets[0])};
        arg.contents_begin = {&vals_pointers[0], 0};
        arg.count = 3;
        return arg;
    }
    contents_arg_t contents_full() const noexcept {
        contents_arg_t arg {};
        arg.presences_begin = &presences;
        arg.offsets_begin = {&offsets[0], sizeof(offsets[0])};
        arg.lengths_begin = {&lengths[0], 0};
        arg.contents_begin = {&vals_pointers[0], 0};
        arg.count = 3;
        return arg;
    }
};

template <typename locations_at>
void round_trip(blobs_ref_gt<locations_at>& ref, triplet_t const& triplet) {
    round_trip(ref, triplet.contents_arrow());
    round_trip(ref, triplet.contents_lengths());
    round_trip(ref, triplet.contents_full());
}

template <template <typename locations_at> class ref_at, typename locations_at>
void check_equalities(ref_at<locations_at>& ref, triplet_t const& triplet) {
    check_equalities(ref, triplet.contents_arrow());
    check_equalities(ref, triplet.contents_lengths());
    check_equalities(ref, triplet.contents_full());
}

void check_binary_collection(blobs_collection_t& collection) {

    triplet_t triplet;
    auto ref = collection[triplet.keys];
    round_trip(ref, triplet);

    // Overwrite those values with same size integers and try again
    for (auto& val : triplet.vals)
        val += 7;
    round_trip(ref, triplet);

    // Overwrite with empty values, but check for existence
    EXPECT_TRUE(ref.clear());
    check_length(ref, 0);

    // Check scans
    keys_range_t present_keys = collection.keys();
    keys_stream_t present_it = present_keys.begin();
    auto expected_it = triplet.keys.begin();
    for (; expected_it != triplet.keys.end(); ++present_it, ++expected_it) {
        EXPECT_EQ(*expected_it, *present_it);
    }
    ++present_it;
    EXPECT_TRUE(present_it.is_end());

    // Remove all of the values and check that they are missing
    EXPECT_TRUE(ref.erase());
    check_length(ref, ustore_length_missing_k);
}

/**
 * Try opening a DB, clearing it, accessing the main collection.
 * Write some data into that main collection, and test retrieving it.
 */
TEST(db, open_clear_close) {

    clear_environment();
    database_t db;
    EXPECT_TRUE(db.open(config().c_str()));
    EXPECT_TRUE(db.clear());

    // Try getting the main collection
    blobs_collection_t collection = db.main();
    check_binary_collection(collection);
}

/**
 * Insert data into main collection.
 * Clear the whole DBMS.
 * Make sure the main collection is empty.
 */
TEST(db, clear_collection_by_clearing_db) {

    clear_environment();
    database_t db;
    EXPECT_TRUE(db.open(config().c_str()));

    blobs_collection_t collection = db.main();
    triplet_t triplet;
    auto ref = collection[triplet.keys];
    round_trip(ref, triplet.contents_arrow());

    EXPECT_EQ(collection.keys().size(), 3ul);
    EXPECT_EQ(collection.items().size(), 3ul);

    // Overwrite with empty values, but check for existence
    EXPECT_TRUE(db.clear());
    check_length(ref, ustore_length_missing_k);
}

/**
 * Fill the main collection with some keys from 1000 to 1100 and from 900 to 800.
 * Overwrite some of those with larger values, checking consistency.
 */
TEST(db, overwrite_with_step) {

    clear_environment();
    database_t db;
    EXPECT_TRUE(db.open(config().c_str()));
    EXPECT_TRUE(db.clear());

    // Try getting the main collection
    blobs_collection_t collection = db.main();

    // Monotonically increasing
    for (ustore_key_t k = 1000; k != 1100; ++k)
        collection[k] = "some";
    for (ustore_key_t k = 1000; k != 1100; ++k)
        EXPECT_EQ(*collection[k].value(), "some");

    EXPECT_EQ(collection.keys().size(), 100ul);
    EXPECT_EQ(collection.items().size(), 100ul);

    // Monotonically decreasing
    for (ustore_key_t k = 900; k != 800; --k)
        collection[k] = "other";
    for (ustore_key_t k = 900; k != 800; --k)
        EXPECT_EQ(*collection[k].value(), "other");

    EXPECT_EQ(collection.keys().size(), 200ul);
    EXPECT_EQ(collection.items().size(), 200ul);

    // Overwrites and new entries between two ranges
    for (ustore_key_t k = 800; k != 1100; k += 2)
        collection[k] = "third";
    for (ustore_key_t k = 800; k != 1100; k += 2)
        EXPECT_EQ(*collection[k].value(), "third");

    EXPECT_EQ(collection.keys().size(), 250ul);
    EXPECT_EQ(collection.items().size(), 250ul);
}

/**
 * Populate the main collection, close the DBMS, reopen it, check consistency.
 */
TEST(db, persistency) {

    if (!path())
        return;

    clear_environment();
    database_t db;
    EXPECT_TRUE(db.open(config().c_str()));

    triplet_t triplet;
    {
        blobs_collection_t main_collection = db.main();
        auto main_collection_ref = main_collection[triplet.keys];
        check_length(main_collection_ref, ustore_length_missing_k);
        round_trip(main_collection_ref, triplet);
        check_length(main_collection_ref, triplet_t::val_size_k);

        if (ustore_supports_named_collections_k) {
            blobs_collection_t named_collection = *db.create("collection");
            auto named_collection_ref = named_collection[triplet.keys];
            check_length(named_collection_ref, ustore_length_missing_k);
            round_trip(named_collection_ref, triplet);
            check_length(named_collection_ref, triplet_t::val_size_k);
            EXPECT_TRUE(named_collection.clear_values());
            check_length(named_collection_ref, 0);
        }
    }
    db.close();
    {
        EXPECT_TRUE(db.open(config().c_str()));

        blobs_collection_t main_collection = db.main();
        auto main_collection_ref = main_collection[triplet.keys];
        check_equalities(main_collection_ref, triplet);
        check_length(main_collection_ref, triplet_t::val_size_k);
        EXPECT_EQ(main_collection.keys().size(), 3ul);
        EXPECT_EQ(main_collection.items().size(), 3ul);

        if (ustore_supports_named_collections_k) {
            EXPECT_TRUE(db.contains("collection"));
            EXPECT_TRUE(*db.contains("collection"));
            blobs_collection_t named_collection = *db["collection"];
            auto named_collection_ref = named_collection[triplet.keys];
            check_length(named_collection_ref, 0);
            EXPECT_EQ(named_collection.keys().size(), 3ul);
            EXPECT_EQ(named_collection.items().size(), 3ul);
        }
    }
}

/**
 * Creates news collections under unique names.
 * Tests collection lookup by name, dropping/clearing existing collections.
 */
TEST(db, named_collections) {

    clear_environment();
    database_t db;
    EXPECT_TRUE(db.open(config().c_str()));

    // We can't drop a missing collection, or the main one.
    EXPECT_FALSE(*db.contains("unknown"));
    EXPECT_FALSE(db.drop("unknown"));
    EXPECT_FALSE(db.drop(""));

    if (ustore_supports_named_collections_k) {

        EXPECT_TRUE(db["col1"]);
        EXPECT_TRUE(db["col2"]);

        EXPECT_FALSE(db.create("col1"));
        blobs_collection_t col1 = *db["col1"];
        EXPECT_FALSE(db.create("col2"));
        blobs_collection_t col2 = *db["col2"];

        check_binary_collection(col1);
        check_binary_collection(col2);

        EXPECT_TRUE(db.drop("col1"));
        EXPECT_TRUE(db.drop("col2"));
        EXPECT_TRUE(*db.contains(""));
        EXPECT_FALSE(*db.contains("col1"));
        EXPECT_FALSE(*db.contains("col2"));
    }

    EXPECT_TRUE(db.clear());
    EXPECT_TRUE(*db.contains(""));
}

/**
 * Tests listing the names of present collections.
 */
TEST(db, named_collections_list) {

    if (!ustore_supports_named_collections_k)
        return;

    clear_environment();
    database_t db;
    EXPECT_TRUE(db.open(config().c_str()));

    blobs_collection_t col1 = *db.create("col1");
    blobs_collection_t col2 = *db.create("col2");
    blobs_collection_t col3 = *db.create("col3");
    blobs_collection_t col4 = *db.create("col4");

    EXPECT_TRUE(*db.contains("col1"));
    EXPECT_TRUE(*db.contains("col2"));

    auto maybe_txn = db.transact();
    EXPECT_TRUE(maybe_txn);
    auto maybe_cols = maybe_txn->collections();
    EXPECT_TRUE(maybe_cols);

    size_t count = 0;
    std::vector<std::string> collections;
    auto cols = *maybe_cols;
    while (!cols.names.is_end()) {
        collections.push_back(std::string(*cols.names));
        ++cols.names;
        ++count;
    }
    EXPECT_EQ(count, 4);
    std::sort(collections.begin(), collections.end());
    EXPECT_EQ(collections[0], "col1");
    EXPECT_EQ(collections[1], "col2");
    EXPECT_EQ(collections[2], "col3");
    EXPECT_EQ(collections[3], "col4");

    EXPECT_TRUE(db.drop("col1"));
    EXPECT_FALSE(*db.contains("col1"));
    EXPECT_FALSE(db.drop(""));
    EXPECT_TRUE(db.main().clear());
}

/**
 * Tests clearing values in a collection, which would preserve the keys,
 * but empty the binary strings.
 */
TEST(db, clear_values) {
    clear_environment();
    database_t db;
    EXPECT_TRUE(db.open(config().c_str()));

    triplet_t triplet;
    blobs_collection_t col = db.main();
    auto collection_ref = col[triplet.keys];

    check_length(collection_ref, ustore_length_missing_k);
    round_trip(collection_ref, triplet);
    check_length(collection_ref, triplet_t::val_size_k);

    EXPECT_TRUE(col.clear_values());
    check_length(collection_ref, 0);
    EXPECT_TRUE(col.clear());
    check_length(collection_ref, ustore_length_missing_k);

    EXPECT_TRUE(db.clear());
}

/**
 * Tests presences with C and C++ Interfaces.
 */
TEST(db, presences) {
    clear_environment();
    database_t db;
    EXPECT_TRUE(db.open(config().c_str()));
    auto main = db.main();

    constexpr std::size_t keys_count = 1000;
    for (std::size_t i = 0; i != keys_count; ++i) {
        if (i % 10)
            main[i] = "value";
    }

    // Native C Interface
    std::vector<ustore_key_t> keys(keys_count);
    std::iota(keys.begin(), keys.end(), 0);
    ustore_octet_t* found_presences = nullptr;
    arena_t arena(db);
    status_t status {};
    ustore_read_t read {};
    read.db = db;
    read.error = status.member_ptr();
    read.arena = arena.member_ptr();
    read.tasks_count = keys_count;
    read.keys = keys.data();
    read.keys_stride = sizeof(ustore_key_t);
    read.presences = &found_presences;

    ustore_read(&read);
    EXPECT_TRUE(status);

    for (std::size_t i = 0; i != keys_count; ++i) {
        if (i % 10) {
            EXPECT_TRUE(check_presence(found_presences, i));
        }
        else {
            EXPECT_FALSE(check_presence(found_presences, i));
        }
    }

    // C++ Interface
    auto presences = main[keys].present().throw_or_release();
    for (std::size_t i = 0; i != keys_count; ++i) {
        if (i % 10) {
            EXPECT_TRUE(presences[i]);
        }
        else {
            EXPECT_FALSE(presences[i]);
        }
    }
}

TEST(db, scan) {
    clear_environment();
    database_t db;
    EXPECT_TRUE(db.open(config().c_str()));
    blobs_collection_t collection = db.main();

    constexpr std::size_t keys_size = 1000;
    std::array<ustore_key_t, keys_size> keys;
    std::iota(std::begin(keys), std::end(keys), 0);
    auto ref = collection[keys];
    value_view_t value("value");
    EXPECT_TRUE(ref.assign(value));
    keys_stream_t stream(db, collection, 256);

    EXPECT_TRUE(stream.seek_to_first());
    ustore_key_t key = 0;
    while (!stream.is_end()) {
        EXPECT_EQ(stream.key(), key++);
        ++stream;
    }
    EXPECT_EQ(key, keys_size);
}

/**
 * Ordered batched scan over the main collection.
 */
TEST(db, batch_scan) {

    clear_environment();
    database_t db;
    EXPECT_TRUE(db.open(config().c_str()));
    EXPECT_TRUE(db.clear());
    blobs_collection_t collection = db.main();

    std::array<ustore_key_t, 512> keys;
    std::iota(std::begin(keys), std::end(keys), 0);
    auto ref = collection[keys];
    value_view_t value("value");
    EXPECT_TRUE(ref.assign(value));
    keys_stream_t stream(db, collection, 256);

    EXPECT_TRUE(stream.seek_to_first());
    auto batch = stream.keys_batch();
    EXPECT_EQ(batch.size(), 256);
    EXPECT_FALSE(stream.is_end());
    for (ustore_key_t i = 0; i != 256; ++i)
        EXPECT_EQ(batch[i], i);

    EXPECT_TRUE(stream.seek_to_next_batch());
    batch = stream.keys_batch();
    EXPECT_EQ(batch.size(), 256);
    EXPECT_FALSE(stream.is_end());
    for (ustore_key_t i = 0; i != 256; ++i)
        EXPECT_EQ(batch[i], i + 256);

    EXPECT_TRUE(stream.seek_to_next_batch());
    batch = stream.keys_batch();
    EXPECT_EQ(batch.size(), 0);
    EXPECT_TRUE(stream.is_end());
}

/**
 * Checks the "Read Commited" consistency guarantees of transactions.
 * Readers can't see the contents of pending (not committed) transactions.
 * https://jepsen.io/consistency/models/read-committed
 */
TEST(db, transaction_read_commited) {
    if (!ustore_supports_transactions_k)
        return;

    clear_environment();
    database_t db;
    EXPECT_TRUE(db.open(config().c_str()));
    EXPECT_TRUE(db.transact());
    transaction_t txn = *db.transact();

    triplet_t triplet;

    auto txn_ref = txn[triplet.keys];
    round_trip(txn_ref, triplet);

    blobs_collection_t collection = db.main();
    auto collection_ref = collection[triplet.keys];

    // Check for missing values before commit
    check_length(collection_ref, ustore_length_missing_k);
    EXPECT_TRUE(txn.commit());
    EXPECT_TRUE(txn.reset());

    // Validate that values match after commit
    check_equalities(collection_ref, triplet);
}

/**
 * Checks the "Snapshot Isolation" consistency guarantees of transactions.
 * If needed, readers can initiate snapshot-backed transactions.
 * All the reads, directed to that snapshot will not see newer operations,
 * affecting the HEAD state. From a consistency standpoint, it is a downgrade
 * from "Strictly Serializable" ACID transactions, but it is extremely useful
 * for numerous Business Intelligence applications.
 * https://jepsen.io/consistency/models/snapshot-isolation
 */
TEST(db, transaction_snapshot_isolation) {
    if (!ustore_supports_snapshots_k)
        return;

    clear_environment();
    database_t db;
    EXPECT_TRUE(db.open(config().c_str()));

    triplet_t triplet;
    triplet_t triplet_same_v;
    triplet_same_v.vals = {'D', 'D', 'D'};

    blobs_collection_t collection = db.main();
    auto collection_ref = collection[triplet.keys];

    check_length(collection_ref, ustore_length_missing_k);
    round_trip(collection_ref, triplet);

    auto snap = *db.snapshot();
    auto snap_ref = snap[triplet.keys];
    round_trip(snap_ref, triplet);
    round_trip(collection_ref, triplet_same_v);

    // Validate that values match
    auto maybe_retrieved = snap_ref.value();
    auto const& retrieved = *maybe_retrieved;
    auto it = retrieved.begin();
    auto cont = triplet_same_v.contents_full();
    for (std::size_t i = 0; i != cont.size(); ++i, ++it) {
        auto expected_len = cont[i].size();
        auto expected_begin = cont[i].begin();

        value_view_t retrieved_view = *it;
        value_view_t expected_view(expected_begin, expected_begin + expected_len);
        EXPECT_EQ(retrieved_view.size(), expected_view.size());
        EXPECT_NE(retrieved_view, expected_view);
    }

    snap = *db.snapshot();
    auto ref = snap[triplet_same_v.keys];
    round_trip(ref, triplet_same_v);

    EXPECT_TRUE(db.clear());
}

TEST(db, snapshots_list) {
    if (!ustore_supports_snapshots_k)
        return;

    clear_environment();
    database_t db;
    EXPECT_TRUE(db.open(config().c_str()));

    auto snap_1 = *db.snapshot();
    auto snapshots = snap_1.snapshots();
    auto snaps = *snapshots;

    EXPECT_EQ(snaps.size(), 1u);

    auto snap_2 = *db.snapshot();
    snapshots = snap_2.snapshots();
    snaps = *snapshots;
    EXPECT_EQ(snaps.size(), 2u);

    auto snap_3 = *db.snapshot();
    snapshots = snap_3.snapshots();
    snaps = *snapshots;

    EXPECT_EQ(snaps.size(), 3u);

    snap_1 = *db.snapshot();
    snapshots = snap_1.snapshots();
    snaps = *snapshots;
    EXPECT_EQ(snaps.size(), 3u);

    EXPECT_TRUE(db.clear());

    snapshots = snap_1.snapshots();
    snaps = *snapshots;
    EXPECT_EQ(snaps.size(), 0u);

    EXPECT_TRUE(db.clear());
}

TEST(db, transaction_with_snapshot) {
    if (!ustore_supports_snapshots_k)
        return;

    clear_environment();
    database_t db;
    EXPECT_TRUE(db.open(config().c_str()));

    triplet_t triplet;
    triplet_t triplet_same_v;
    triplet_same_v.vals = {'D', 'D', 'D'};

    blobs_collection_t collection = db.main();
    auto collection_ref = collection[triplet.keys];

    check_length(collection_ref, ustore_length_missing_k);
    round_trip(collection_ref, triplet);

    auto snap = *db.snapshot();
    auto snap_ref = snap[triplet.keys];
    check_equalities(snap_ref, triplet);

    round_trip(collection_ref, triplet_same_v);

    transaction_t txn = *db.transact();
    auto txn_ref_1 = txn[triplet.keys];
    check_equalities(txn_ref_1, triplet_same_v);

    txn.set_snapshot(snap.snap());
    auto txn_ref_2 = txn[triplet.keys];
    check_equalities(txn_ref_2, triplet);

    snap = *db.snapshot();
    txn.set_snapshot(snap.snap());

    auto txn_ref_3 = txn[triplet.keys];
    check_equalities(txn_ref_3, triplet_same_v);
    EXPECT_TRUE(db.clear());
}

TEST(db, set_wrong_snapshot) {
    if (!ustore_supports_snapshots_k)
        return;

    clear_environment();
    database_t db;
    EXPECT_TRUE(db.open(config().c_str()));

    triplet_t triplet;
    blobs_collection_t collection = db.main();
    auto collection_ref = collection[triplet.keys];

    check_length(collection_ref, ustore_length_missing_k);
    round_trip(collection_ref, triplet);

    auto snap = *db.snapshot();

    auto snap_ref = snap[triplet.keys];
    check_equalities(snap_ref, triplet);

    auto snapshots = snap.snapshots();
    auto snaps = *snapshots;
    EXPECT_EQ(snaps.size(), 1u);

    auto snapshot = snap.snap();

    ustore_snapshot_t wrong_snap = 1u;
    snap.set_snapshot(wrong_snap);

    auto wrong_snap_ref = snap[triplet.keys];
    EXPECT_FALSE(wrong_snap_ref.value());

    snap.set_snapshot(snapshot);
    auto right_snap_ref = snap[triplet.keys];
    EXPECT_TRUE(right_snap_ref.value());

    EXPECT_TRUE(db.clear());
}

/**
 * Exports snapshot as a database, loads and checks contents
 */
TEST(db, export_snapshot) {
    if (!ustore_supports_snapshots_k)
        return;

    clear_environment();
    std::string dir = fmt::format("{}/original/", path());
    std::string dir1 = fmt::format("{}/export1/", path());
    std::string dir2 = fmt::format("{}/export2/", path());
    std::filesystem::create_directory(dir);
    std::filesystem::create_directory(dir1);
    std::filesystem::create_directory(dir2);

    database_t db;
    auto config = fmt::format(R"({{"version": "1.0", "directory": "{}"}})", dir);
    EXPECT_TRUE(db.open(config.c_str()));

    triplet_t triplet;
    triplet_t triplet_same_v;
    triplet_same_v.vals = {'D', 'D', 'D'};

    blobs_collection_t collection = db.main();
    auto collection_ref = collection[triplet.keys];
    round_trip(collection_ref, triplet);

    // Export snapshot
    auto snap1 = *db.snapshot();
    EXPECT_TRUE(snap1.export_to(dir1.c_str()));
    check_equalities(collection_ref, triplet);

    // Load exported snapshot
    database_t db1;
    config = fmt::format(R"({{"version": "1.0", "directory": "{}"}})", dir1);
    EXPECT_TRUE(db1.open(config.c_str()));
    // Check values
    auto collection1 = db1.main();
    auto collection_ref1 = collection1[triplet.keys];
    check_equalities(collection_ref1, triplet);

    // Change original
    round_trip(collection_ref, triplet_same_v);

    // Export one more snapshot
    auto snap2 = *db.snapshot();
    EXPECT_TRUE(snap2.export_to(dir2.c_str()));
    check_equalities(collection_ref, triplet_same_v);

    // Load second exported snapshot
    database_t db2;
    config = fmt::format(R"({{"version": "1.0", "directory": "{}"}})", dir2);
    EXPECT_TRUE(db2.open(config.c_str()));
    // Check values
    auto collection2 = db2.main();
    auto collection_ref2 = collection2[triplet_same_v.keys];
    check_equalities(collection_ref2, triplet_same_v);

    // Check snapshots
    check_equalities(collection_ref1, triplet);
    check_equalities(collection_ref2, triplet_same_v);
    // Change snapshots
    round_trip(collection_ref1, triplet_same_v);
    round_trip(collection_ref2, triplet);
    // Check snapshots
    check_equalities(collection_ref1, triplet_same_v);
    check_equalities(collection_ref2, triplet);

    EXPECT_TRUE(db.clear());
    EXPECT_TRUE(db1.clear());
    EXPECT_TRUE(db2.clear());
}

/**
 * Creates news collection under unique names.
 * Fill data in collection. Checking/dropping/checking collection data by thread.
 */
TEST(db, snapshot_with_threads) {
    if (!ustore_supports_snapshots_k)
        return;

    clear_environment();

    database_t db;
    EXPECT_TRUE(db.open(config().c_str()));

    triplet_t triplet;
    triplet_t triplet_same_v;
    triplet_same_v.vals = {'D', 'D', 'D'};

    blobs_collection_t collection = db.main();
    auto ref = collection[triplet.keys];
    round_trip(ref, triplet);

    auto snap = *db.snapshot();
    auto snap_ref = snap[triplet.keys];

    round_trip(ref, triplet_same_v);

    std::shared_mutex mutex;
    bool is_deleted = false;
    auto task_read = [&]() {
        while (true) {
            std::shared_lock _ {mutex};
            if (is_deleted) {
                auto ref = snap[triplet.keys];
                check_equalities(ref, triplet_same_v);
                break;
            }
            check_equalities(snap_ref, triplet);
        }
    };

    auto task_reset = [&]() {
        std::unique_lock _ {mutex};
        snap.set_snapshot(0);
        is_deleted = true;
    };

    std::thread t1(task_read);
    std::thread t2(task_reset);
    t1.join();
    t2.join();

    EXPECT_TRUE(db.clear());
}

TEST(db, transaction_erase_missing) {
    if (!ustore_supports_transactions_k)
        return;

    clear_environment();
    database_t db;
    EXPECT_TRUE(db.open(config().c_str()));
    EXPECT_TRUE(db.transact());
    transaction_t txn1 = *db.transact();
    transaction_t txn2 = *db.transact();

    EXPECT_TRUE(txn2.main().at(-7297309151944849401).erase());
    EXPECT_TRUE(txn1.main().at(-8640850744835793378).erase());
    EXPECT_TRUE(txn1.commit());
    EXPECT_TRUE(txn2.commit());

    EXPECT_EQ(db.main().at(-8640850744835793378).value(), value_view_t {});
    EXPECT_EQ(db.main().at(-7297309151944849401).value(), value_view_t {});
    EXPECT_EQ(db.main().keys().size(), 0u);
}

TEST(db, transaction_write_conflicting) {
    if (!ustore_supports_transactions_k)
        return;

    clear_environment();
    database_t db;
    EXPECT_TRUE(db.open(config().c_str()));
    EXPECT_TRUE(db.transact());
    transaction_t txn1 = *db.transact();
    transaction_t txn2 = *db.transact();

    EXPECT_TRUE(txn2.main().at(6).assign("a"));
    EXPECT_TRUE(txn1.main().at(6).assign("b"));
    EXPECT_TRUE(txn1.commit());
    EXPECT_FALSE(txn2.commit());
}

/**
 *
 */
TEST(db, transaction_sequenced_commit) {
    if (!ustore_supports_transactions_k)
        return;

    clear_environment();
    database_t db;
    EXPECT_TRUE(db.open(config().c_str()));

    EXPECT_TRUE(db.transact());
    transaction_t txn = *db.transact();

    triplet_t triplet;
    auto txn_ref = txn[triplet.keys];

    EXPECT_TRUE(txn_ref.assign(triplet.contents()));
    auto maybe_sequence_number = txn.sequenced_commit();
    EXPECT_TRUE(maybe_sequence_number);
    auto current_sequence_number = *maybe_sequence_number;
    EXPECT_GT(current_sequence_number, 0);
    EXPECT_TRUE(txn.reset());
#if 0
    auto previous_sequence_number = current_sequence_number;
    EXPECT_TRUE(txn_ref.value());
    maybe_sequence_number = txn.sequenced_commit();
    EXPECT_TRUE(maybe_sequence_number);
    current_sequence_number = *maybe_sequence_number;
    EXPECT_EQ(current_sequence_number, previous_sequence_number);
    EXPECT_TRUE(txn.reset());

    previous_sequence_number = current_sequence_number;
    EXPECT_TRUE(txn_ref.assign(triplet.contents()));
    maybe_sequence_number = txn.sequenced_commit();
    EXPECT_TRUE(maybe_sequence_number);
    current_sequence_number = *maybe_sequence_number;
    EXPECT_GT(current_sequence_number, previous_sequence_number);
#endif
}

#pragma region Paths Modality

/**
 * Tests "Paths" Modality, with variable length keys.
 * Reads, writes, prefix matching and pattern matching.
 */
TEST(db, paths) {

    clear_environment();
    database_t db;
    EXPECT_TRUE(db.open(config().c_str()));

    char const* keys[] {"Facebook", "Apple", "Amazon", "Netflix", "Google", "Nvidia", "Adobe"};
    char const* vals[] {"F", "A", "A", "N", "G", "N", "A"};
    std::size_t keys_count = sizeof(keys) / sizeof(keys[0]);
    ustore_char_t separator = '\0';

    arena_t arena(db);
    status_t status {};
    ustore_paths_write_t paths_write {};
    paths_write.db = db;
    paths_write.error = status.member_ptr();
    paths_write.arena = arena.member_ptr();
    paths_write.tasks_count = keys_count;
    paths_write.path_separator = separator;
    paths_write.paths = keys;
    paths_write.paths_stride = sizeof(char const*);
    paths_write.values_bytes = reinterpret_cast<ustore_bytes_cptr_t*>(vals);
    paths_write.values_bytes_stride = sizeof(char const*);

    ustore_paths_write(&paths_write);
    char* vals_recovered {};
    ustore_paths_read_t paths_read {};
    paths_read.db = db;
    paths_read.error = status.member_ptr();
    paths_read.arena = arena.member_ptr();
    paths_read.tasks_count = keys_count;
    paths_read.path_separator = separator;
    paths_read.paths = keys;
    paths_read.paths_stride = sizeof(char const*);
    paths_read.values = reinterpret_cast<ustore_bytes_ptr_t*>(&vals_recovered);

    ustore_paths_read(&paths_read);
    EXPECT_TRUE(status);
    EXPECT_EQ(std::string_view(vals_recovered, keys_count * 2),
              std::string_view("F\0A\0A\0N\0G\0N\0A\0", keys_count * 2));

    // Try getting either "Netflix" or "Nvidia" as one of the keys with "N" prefix
    ustore_str_view_t prefix = "N";
    ustore_length_t max_count = 1;
    ustore_length_t* results_counts {};
    ustore_length_t* tape_offsets {};
    ustore_char_t* tape_begin {};
    ustore_paths_match_t paths_match {};
    paths_match.db = db;
    paths_match.error = status.member_ptr();
    paths_match.arena = arena.member_ptr();
    paths_match.tasks_count = 1;
    paths_match.match_counts_limits = &max_count;
    paths_match.patterns = &prefix;
    paths_match.match_counts = &results_counts;
    paths_match.paths_offsets = &tape_offsets;
    paths_match.paths_strings = &tape_begin;

    ustore_paths_match(&paths_match);
    auto first_match_for_a = std::string_view(tape_begin);
    EXPECT_EQ(results_counts[0], 1);
    EXPECT_TRUE(first_match_for_a == "Netflix" || first_match_for_a == "Nvidia");

    // Try getting the remaining results, which is the other one from that same pair
    max_count = 10;
    paths_match.previous = &tape_begin;
    paths_match.options = ustore_option_dont_discard_memory_k;
    ustore_paths_match(&paths_match);
    auto second_match_for_a = std::string_view(tape_begin);
    EXPECT_EQ(results_counts[0], 1);
    EXPECT_TRUE(second_match_for_a == "Netflix" || second_match_for_a == "Nvidia");
    EXPECT_NE(first_match_for_a, second_match_for_a);

    // Try performing parallel queries in the same collection
    ustore_str_view_t prefixes[2] = {"A", "N"};
    std::size_t prefixes_count = sizeof(prefixes) / sizeof(prefixes[0]);
    max_count = 10;
    paths_match.tasks_count = prefixes_count;
    paths_match.patterns = prefixes;
    paths_match.patterns_stride = sizeof(ustore_str_view_t);
    paths_match.previous = nullptr;
    ustore_paths_match(&paths_match);
    auto total_count = std::accumulate(results_counts, results_counts + prefixes_count, 0ul);
    strings_tape_iterator_t tape_iterator {total_count, tape_begin};
    std::set<std::string> tape_parts;
    while (!tape_iterator.is_end()) {
        tape_parts.insert(*tape_iterator);
        ++tape_iterator;
    }
    EXPECT_EQ(results_counts[0], 3);
    EXPECT_EQ(results_counts[1], 2);
    EXPECT_NE(tape_parts.find("Netflix"), tape_parts.end());
    EXPECT_NE(tape_parts.find("Adobe"), tape_parts.end());

    // Now try matching a Regular Expression
    prefix = "Netflix|Google";
    paths_match.tasks_count = 1;
    paths_match.patterns = &prefix;
    ustore_paths_match(&paths_match);
    first_match_for_a = std::string_view(tape_begin);
    second_match_for_a = std::string_view(tape_begin + tape_offsets[1]);
    EXPECT_EQ(results_counts[0], 2);
    EXPECT_TRUE(first_match_for_a == "Netflix" || first_match_for_a == "Google");
    EXPECT_TRUE(second_match_for_a == "Netflix" || second_match_for_a == "Google");

    // Try a more complex regular expression
    prefix = "A.*e";
    ustore_paths_match(&paths_match);
    first_match_for_a = std::string_view(tape_begin);
    second_match_for_a = std::string_view(tape_begin + tape_offsets[1]);
    EXPECT_EQ(results_counts[0], 2);
    EXPECT_TRUE(first_match_for_a == "Apple" || first_match_for_a == "Adobe");
    EXPECT_TRUE(second_match_for_a == "Apple" || second_match_for_a == "Adobe");

    // Existing single letter prefix
    prefix = "A";
    ustore_paths_match(&paths_match);
    first_match_for_a = std::string_view(tape_begin);
    second_match_for_a = std::string_view(tape_begin + tape_offsets[1]);
    EXPECT_EQ(results_counts[0], 3);
    EXPECT_TRUE(first_match_for_a == "Apple" || first_match_for_a == "Adobe" || first_match_for_a == "Amazon");
    EXPECT_TRUE(second_match_for_a == "Apple" || second_match_for_a == "Adobe" || second_match_for_a == "Amazon");

    // Missing single letter prefix
    prefix = "X";
    ustore_paths_match(&paths_match);
    EXPECT_EQ(results_counts[0], 0);
    EXPECT_EQ(*paths_match.error, nullptr);

    // Missing pattern
    prefix = "X.*";
    ustore_paths_match(&paths_match);
    EXPECT_EQ(results_counts[0], 0);
    EXPECT_EQ(*paths_match.error, nullptr);

    // Try a more complex regular expression
    prefix = "oo:18:\\*";
    ustore_paths_match(&paths_match);
    EXPECT_EQ(results_counts[0], 0);
    EXPECT_EQ(*paths_match.error, nullptr);

    // Try an even more complex regular expression
    prefix = "oo:18:\\\\*";
    ustore_paths_match(&paths_match);
    EXPECT_EQ(results_counts[0], 0);
    EXPECT_EQ(*paths_match.error, nullptr);

    EXPECT_TRUE(db.clear());

    // Try an even more complex regular expression on empty DB
    prefix = "oo:18:\\\\*";
    ustore_paths_match(&paths_match);
    EXPECT_EQ(results_counts[0], 0);
    EXPECT_EQ(*paths_match.error, nullptr);
}

/**
 * Tests "Paths" Modality, by forming bidirectional linked lists from string-to-string mappings.
 * Uses different-length unique strings. As the underlying modality may be implemented as a bucketed hash-map,
 * this test helps catch problems in bucket reorganization.
 */
TEST(db, paths_linked_list) {
    constexpr std::size_t count = 1000;
    clear_environment();
    database_t db;
    EXPECT_TRUE(db.open(config().c_str()));

    arena_t arena(db);
    ustore_char_t separator = '\0';
    status_t status;

    ustore_paths_write_t paths_write {};
    paths_write.db = db;
    paths_write.error = status.member_ptr();
    paths_write.arena = arena.member_ptr();
    paths_write.tasks_count = 1;
    paths_write.path_separator = separator;

    ustore_paths_read_t paths_read {};
    paths_read.db = db;
    paths_read.error = status.member_ptr();
    paths_read.arena = arena.member_ptr();
    paths_read.tasks_count = 1;
    paths_read.path_separator = separator;

    // Generate some random strings for our tests
    constexpr auto alphabet = "abcdefghijklmnop";
    auto make_random_str = []() {
        auto str = std::string();
        auto len = static_cast<std::size_t>(std::rand() % 100) + 8;
        for (std::size_t i = 0; i != len; ++i)
            str.push_back(alphabet[std::rand() % 16]);
        return str;
    };
    std::set<std::string> unique;
    while (unique.size() != count)
        unique.insert(make_random_str());

    // Lets form a linked list, where every key maps into the the next key.
    // Then we will traverse the linked list from start to end.
    // Then we will re-link it in reverse order and traverse again.
    std::vector<ustore_str_view_t> begins(unique.size());
    std::transform(unique.begin(), unique.end(), begins.begin(), [](std::string const& str) { return str.c_str(); });

    // Link forward
    for (std::size_t i = 0; i + 1 != begins.size(); ++i) {
        ustore_str_view_t smaller = begins[i];
        ustore_str_view_t bigger = begins[i + 1];
        paths_write.paths = &smaller;
        paths_write.values_bytes = reinterpret_cast<ustore_bytes_cptr_t*>(&bigger);
        ustore_paths_write(&paths_write);
        EXPECT_TRUE(status);

        // Check if it was successfully written:
        ustore_str_span_t bigger_received = nullptr;
        paths_read.paths = &smaller;
        paths_read.values = reinterpret_cast<ustore_bytes_ptr_t*>(&bigger_received);
        ustore_paths_read(&paths_read);
        EXPECT_TRUE(status);
        EXPECT_EQ(std::string_view(bigger), std::string_view(bigger_received));
    }

    // Traverse forward, counting the entries and checking the order
    for (std::size_t i = 0; i + 1 != begins.size(); ++i) {
        ustore_str_view_t smaller = begins[i];
        ustore_str_view_t bigger = begins[i + 1];
        ustore_str_span_t bigger_received = nullptr;
        paths_read.paths = &smaller;
        paths_read.values = reinterpret_cast<ustore_bytes_ptr_t*>(&bigger_received);
        ustore_paths_read(&paths_read);
        EXPECT_TRUE(status);
        EXPECT_EQ(std::string_view(bigger), std::string_view(bigger_received));
    }

    // Re-link in reverse order
    for (std::size_t i = 0; i + 1 != begins.size(); ++i) {
        ustore_str_view_t smaller = begins[i];
        ustore_str_view_t bigger = begins[i + 1];
        paths_write.paths = &bigger;
        paths_write.values_bytes = reinterpret_cast<ustore_bytes_cptr_t*>(&smaller);
        ustore_paths_write(&paths_write);
        EXPECT_TRUE(status);

        // Check if it was successfully over-written:
        ustore_str_span_t smaller_received = nullptr;
        paths_read.paths = &bigger;
        paths_read.values = reinterpret_cast<ustore_bytes_ptr_t*>(&smaller_received);
        ustore_paths_read(&paths_read);
        EXPECT_TRUE(status);
        EXPECT_EQ(std::string_view(smaller), std::string_view(smaller_received));
    }

    // Traverse backwards, counting the entries and checking the order
    for (std::size_t i = 0; i + 1 != begins.size(); ++i) {
        ustore_str_view_t smaller = begins[i];
        ustore_str_view_t bigger = begins[i + 1];
        ustore_str_span_t smaller_received = nullptr;
        paths_read.paths = &bigger;
        paths_read.values = reinterpret_cast<ustore_bytes_ptr_t*>(&smaller_received);
        ustore_paths_read(&paths_read);
        EXPECT_TRUE(status);
        EXPECT_EQ(std::string_view(smaller), std::string_view(smaller_received));
    }
}

#pragma region Documents Modality

std::vector<std::string> make_three_flat_docs() {
    auto json1 = R"( {"person": "Alice", "age": 24} )"_json.dump();
    auto json2 = R"( {"person": "Bob", "age": 25} )"_json.dump();
    auto json3 = R"( {"person": "Carl", "age": 26} )"_json.dump();
    return {json1, json2, json3};
}

std::vector<std::string> make_three_nested_docs() {
    auto json1 = R"( {"person": {"name":"Alice", "age": 24}} )"_json.dump();
    auto json2 = R"( {"person": [{"name":"Bob", "age": 25}]} )"_json.dump();
    auto json3 = R"( {"person": "Carl", "age": 26} )"_json.dump();
    return {json1, json2, json3};
}

/**
 * Tests "Documents" Modality, mapping integers to structured hierarchical documents.
 * Takes a basic flat JSON document, and checks if it can be imported in JSON, BSON
 * and MessagePack forms, and later be properly accessed at field-level.
 */
TEST(db, docs_flat) {

    clear_environment();
    database_t db;
    EXPECT_TRUE(db.open(config().c_str()));

    // JSON
    docs_collection_t collection = db.main<docs_collection_t>();
    auto jsons = make_three_flat_docs();
    collection[1] = jsons[0].c_str();
    collection[2] = jsons[1].c_str();
    collection[3] = jsons[2].c_str();
    M_EXPECT_EQ_JSON(*collection[1].value(), jsons[0]);
    M_EXPECT_EQ_JSON(*collection[ckf(2, "person")].value(), "\"Bob\"");
    M_EXPECT_EQ_JSON(*collection[ckf(3, "age")].value(), "26");

    // Binary
    auto maybe_person = collection[ckf(1, "person")].value(ustore_doc_field_str_k);
    EXPECT_EQ(std::string_view(maybe_person->c_str(), maybe_person->size()), std::string_view("Alice"));

    // BSON
    bson_error_t error;
    bson_t* bson = bson_new_from_json((uint8_t*)jsons[0].c_str(), -1, &error);
    uint8_t const* buffer = bson_get_data(bson);
    auto view = value_view_t(buffer, bson->len);
    collection.at(4, ustore_doc_field_bson_k) = view;
    M_EXPECT_EQ_JSON(*collection[4].value(), jsons[0]);
    M_EXPECT_EQ_JSON(*collection[ckf(4, "person")].value(), "\"Alice\"");
    M_EXPECT_EQ_JSON(*collection[ckf(4, "age")].value(), "24");
    bson_clear(&bson);

    // MsgPack
    auto message_pack = *collection[1].value(ustore_doc_field_msgpack_k);
    collection.at(5, ustore_doc_field_msgpack_k) = message_pack;
    M_EXPECT_EQ_JSON(*collection[5].value(), jsons[0]);
    M_EXPECT_EQ_JSON(*collection[ckf(5, "person")].value(), "\"Alice\"");
    M_EXPECT_EQ_JSON(*collection[ckf(5, "age")].value(), "24");
}

/**
 * Tries adding 3 simple nested JSONs, using JSON-Pointers
 * to retrieve specific fields across multiple keys.
 */
TEST(db, docs_nested_batch) {

    clear_environment();
    database_t db;
    EXPECT_TRUE(db.open(config().c_str()));
    docs_collection_t collection = db.main<docs_collection_t>();

    auto jsons = make_three_nested_docs();
    std::string continuous_jsons = jsons[0] + jsons[1] + jsons[2];
    auto vals_begin = reinterpret_cast<ustore_bytes_ptr_t>(continuous_jsons.data());
    std::array<ustore_length_t, 4> offsets = {
        0,
        static_cast<ustore_length_t>(jsons[0].size()),
        static_cast<ustore_length_t>(jsons[0].size() + jsons[1].size()),
        static_cast<ustore_length_t>(jsons[0].size() + jsons[1].size() + jsons[2].size()),
    };
    contents_arg_t values {};
    values.offsets_begin = {offsets.data(), sizeof(ustore_length_t)};
    values.contents_begin = {&vals_begin, 0};

    std::array<ustore_key_t, 3> keys = {1, 2, 3};
    auto ref = collection[keys];
    EXPECT_TRUE(ref.assign(values));

    // Read One By One
    M_EXPECT_EQ_JSON(*collection[1].value(), jsons[0]);
    M_EXPECT_EQ_JSON(*collection[2].value(), jsons[1]);
    M_EXPECT_EQ_JSON(*collection[3].value(), jsons[2]);

    auto expected = R"({"name":"Alice", "age": 24})"_json.dump();
    M_EXPECT_EQ_JSON(*collection[ckf(1, "person")].value(), expected);

    expected = R"([{"name":"Bob", "age": 25}])"_json.dump();
    M_EXPECT_EQ_JSON(*collection[ckf(2, "person")].value(), expected);
    M_EXPECT_EQ_JSON(*collection[ckf(2, "/person/0/name")].value(), "\"Bob\"");

    // Read sorted keys
    check_equalities(ref, values);

    // Read not sorted keys
    std::array<ustore_key_t, 3> not_sorted_keys = {1, 3, 2};
    auto not_sorted_ref = collection[not_sorted_keys];
    std::string not_sorted_jsons = jsons[0] + jsons[2] + jsons[1];
    vals_begin = reinterpret_cast<ustore_bytes_ptr_t>(not_sorted_jsons.data());
    offsets[2] = jsons[0].size() + jsons[2].size();
    offsets[3] = jsons[0].size() + jsons[2].size() + jsons[1].size();
    check_equalities(not_sorted_ref, values);

    // Read duplicate keys
    std::array<ustore_key_t, 3> duplicate_keys = {1, 2, 1};
    auto duplicate_ref = collection[duplicate_keys];
    std::string duplicate_jsons = jsons[0] + jsons[1] + jsons[0];
    vals_begin = reinterpret_cast<ustore_bytes_ptr_t>(duplicate_jsons.data());
    offsets[2] = jsons[0].size() + jsons[1].size();
    offsets[3] = jsons[0].size() + jsons[1].size() + jsons[0].size();
    check_equalities(duplicate_ref, values);

    // Read with fields
    std::array<collection_key_field_t, 3> keys_with_fields = {
        ckf(1, "person"),
        ckf(2, "/person/0/name"),
        ckf(3, "age"),
    };
    auto ref_with_fields = collection[keys_with_fields];
    auto field_value1 = R"({"name":"Alice", "age": 24})"_json.dump();
    auto field_value2 = R"("Bob")"_json.dump();
    auto field_value3 = R"(26)"_json.dump();
    std::string field_values = field_value1 + field_value2 + field_value3;
    vals_begin = reinterpret_cast<ustore_bytes_ptr_t>(field_values.data());
    offsets[1] = field_value1.size();
    offsets[2] = field_value1.size() + field_value2.size();
    offsets[3] = field_value1.size() + field_value2.size() + field_value3.size();
    check_equalities(ref_with_fields, values);

    // Check Invalid Json Write
    std::string invalid_json = R"({"name":"Alice", } "age": 24})";
    offsets[1] = jsons[0].size();
    offsets[2] = jsons[0].size() + jsons[1].size();
    offsets[3] = jsons[0].size() + jsons[1].size() + invalid_json.size();
    continuous_jsons = jsons[0] + jsons[1] + invalid_json;
    vals_begin = reinterpret_cast<ustore_bytes_ptr_t>(continuous_jsons.data());

    EXPECT_FALSE(ref.assign(values));
}

/**
 * Performs basic JSON Paths, JSON Merge-Patches, and sub-document level updates.
 */
TEST(db, docs_modify) {
    clear_environment();
    database_t db;
    EXPECT_TRUE(db.open(config().c_str()));
    docs_collection_t collection = db.main<docs_collection_t>();
    auto jsons = make_three_nested_docs();
    collection[1] = jsons[0].c_str();
    M_EXPECT_EQ_JSON(*collection[1].value(), jsons[0]);

    // Update
    auto modifier = R"( {"person": {"name":"Charles", "age": 28}} )"_json.dump();
    EXPECT_TRUE(collection[1].update(modifier.c_str()));
    auto result = collection[1].value();
    M_EXPECT_EQ_JSON(result->c_str(), modifier.c_str());

    // Update By Field
    modifier = R"( {"name": "Alice", "age": 24} )"_json.dump();
    EXPECT_TRUE(collection[ckf(1, "/person")].update(modifier.c_str()));
    result = collection[1].value();
    M_EXPECT_EQ_JSON(result->c_str(), jsons[0].c_str());

    // Insert
    EXPECT_FALSE(collection[1].insert(jsons[1].c_str()));
    EXPECT_TRUE(collection[2].insert(jsons[1].c_str()));
    result = collection[2].value();
    M_EXPECT_EQ_JSON(result->c_str(), jsons[1].c_str());

    // Insert By Field
    modifier = R"("Doe" )"_json.dump();
    auto expected = R"({"person": [{"name":"Bob", "age": 25, "surname" : "Doe"}]})"_json.dump();
    EXPECT_TRUE(collection[ckf(2, "/person/0/surname")].insert(modifier.c_str()));
    result = collection[2].value();
    M_EXPECT_EQ_JSON(result->c_str(), expected.c_str());

    // Upsert
    EXPECT_TRUE(collection[1].upsert(jsons[2].c_str()));
    result = collection[1].value();
    M_EXPECT_EQ_JSON(result->c_str(), jsons[2].c_str());

    // Upsert By Field
    modifier = R"("Charles")"_json.dump();
    expected = R"( {"person": "Charles", "age": 26} )"_json.dump();
    EXPECT_TRUE(collection[ckf(1, "/person")].upsert(modifier.c_str()));
    result = collection[1].value();
    M_EXPECT_EQ_JSON(result->c_str(), expected.c_str());

    modifier = R"(70)"_json.dump();
    expected = R"( {"person": "Charles", "age": 26, "weight" : 70} )"_json.dump();
    EXPECT_TRUE(collection[ckf(1, "/weight")].upsert(modifier.c_str()));
    result = collection[1].value();
    M_EXPECT_EQ_JSON(result->c_str(), expected.c_str());
}

/**
 * Uses a well-known repository of JSON-Patches and JSON-MergePatches,
 * to validate that document modifications work adequately in corner cases.
 */
TEST(db, docs_merge_and_patch) {
    using json_t = nlohmann::json;
    clear_environment();
    database_t db;
    EXPECT_TRUE(db.open(config().c_str()));
    docs_collection_t collection = db.main<docs_collection_t>();

    std::ifstream f_patch("tests/patch.json");
    json_t j_object = json_t::parse(f_patch);
    for (auto it : j_object) {
        auto doc = it["doc"].dump();
        auto patch = it["patch"].dump();
        auto expected = it["expected"].dump();
        collection[1] = doc.c_str();
        EXPECT_TRUE(collection[1].patch(patch.c_str()));
        auto maybe_value = collection[1].value();
        EXPECT_TRUE(maybe_value);
        M_EXPECT_EQ_JSON(maybe_value->c_str(), expected.c_str());
    }

    std::ifstream f_merge("tests/merge.json");
    j_object = json_t::parse(f_merge);
    for (auto it : j_object) {
        auto doc = it["doc"].dump();
        auto merge = it["merge"].dump();
        auto expected = it["expected"].dump();
        collection[1] = doc.c_str();
        EXPECT_TRUE(collection[1].merge(merge.c_str()));
        auto maybe_value = collection[1].value();
        EXPECT_TRUE(maybe_value);
        M_EXPECT_EQ_JSON(maybe_value->c_str(), expected.c_str());
    }
}

/**
 * Fills document collection with info about Alice, Bob and Carl,
 * sampling it later in a form of a table, using both low-level APIs,
 * and higher-level compile-time C++ meta-programming abstractions.
 */
TEST(db, docs_table) {
    using json_t = nlohmann::json;
    clear_environment();
    database_t db;
    EXPECT_TRUE(db.open(config().c_str()));

    // Inject basic data
    docs_collection_t collection = db.main<docs_collection_t>();
    auto json_alice = R"( { "person": "Alice", "age": 27, "height": 1 } )"_json.dump();
    auto json_bob = R"( { "person": "Bob", "age": "27", "weight": 2 } )"_json.dump();
    auto json_carl = R"( { "person": "Carl", "age": 24 } )"_json.dump();
    collection[1] = json_alice.c_str();
    collection[2] = json_bob.c_str();
    collection[3] = json_carl.c_str();
    M_EXPECT_EQ_JSON(*collection[1].value(), json_alice.c_str());
    M_EXPECT_EQ_JSON(*collection[2].value(), json_bob.c_str());

    // Just column names
    {
        auto maybe_fields = collection[1].gist();
        auto fields = *maybe_fields;

        std::vector<std::string> parsed;
        for (auto field : fields)
            parsed.emplace_back(field.data());

        EXPECT_NE(std::find(parsed.begin(), parsed.end(), "/person"), parsed.end());
        EXPECT_NE(std::find(parsed.begin(), parsed.end(), "/height"), parsed.end());
        EXPECT_NE(std::find(parsed.begin(), parsed.end(), "/age"), parsed.end());
        EXPECT_EQ(std::find(parsed.begin(), parsed.end(), "/weight"), parsed.end());
    }

    // Single cell
    {
        auto header = table_header().with<std::uint32_t>("age");
        auto maybe_table = collection[1].gather(header);
        auto table = *maybe_table;
        auto col0 = table.column<0>();

        EXPECT_EQ(col0[0].value, 27);
        EXPECT_FALSE(col0[0].converted);
    }

    // Single row
    {
        auto header = table_header() //
                          .with<std::uint32_t>("age")
                          .with<std::int32_t>("age")
                          .with<std::string_view>("age");

        auto maybe_table = collection[1].gather(header);
        auto table = *maybe_table;
        auto col0 = table.column<0>();
        auto col1 = table.column<1>();
        auto col2 = table.column<2>();

        EXPECT_EQ(col0[0].value, 27);
        EXPECT_FALSE(col0[0].converted);
        EXPECT_EQ(col1[0].value, 27);
        EXPECT_TRUE(col1[0].converted);
        EXPECT_STREQ(col2[0].value.data(), "27");
        EXPECT_TRUE(col2[0].converted);
    }

    // Single column
    {
        auto header = table_header().with<std::int32_t>("age");
        auto maybe_table = collection[{1, 2, 3, 123456}].gather(header);
        auto table = *maybe_table;
        auto col0 = table.column<0>();

        EXPECT_EQ(col0[0].value, 27);
        EXPECT_EQ(col0[1].value, 27);
        EXPECT_TRUE(col0[1].converted);
        EXPECT_EQ(col0[2].value, 24);
    }

    // Single strings column
    {
        auto header = table_header().with<std::string_view>("age");
        auto maybe_table = collection[{1, 2, 3, 123456}].gather(header);
        auto table = *maybe_table;
        auto col0 = table.column<0>();

        EXPECT_STREQ(col0[0].value.data(), "27");
        EXPECT_TRUE(col0[0].converted);
        EXPECT_STREQ(col0[1].value.data(), "27");
        EXPECT_STREQ(col0[2].value.data(), "24");
    }

    // Multi-column
    {
        auto header = table_header() //
                          .with<std::int32_t>("age")
                          .with<std::string_view>("age")
                          .with<std::string_view>("person")
                          .with<float>("person")
                          .with<std::int32_t>("height")
                          .with<std::uint64_t>("weight");

        auto maybe_table = collection[{1, 2, 3, 123456, 654321}].gather(header);
        auto table = *maybe_table;
        auto col0 = table.column<0>();
        auto col1 = table.column<1>();
        auto col2 = table.column<2>();
        auto col3 = table.column<3>();
        auto col4 = table.column<4>();
        auto col5 = table.column<5>();

        EXPECT_EQ(col0[0].value, 27);
        EXPECT_EQ(col0[1].value, 27);
        EXPECT_TRUE(col0[1].converted);
        EXPECT_EQ(col0[2].value, 24);

        EXPECT_STREQ(col1[0].value.data(), "27");
        EXPECT_TRUE(col1[0].converted);
        EXPECT_STREQ(col1[1].value.data(), "27");
        EXPECT_STREQ(col1[2].value.data(), "24");
    }

    // Multi-column Type-punned exports
    {
        table_header_t header {{
            field_type_t {"age", ustore_doc_field_i32_k},
            field_type_t {"age", ustore_doc_field_str_k},
            field_type_t {"person", ustore_doc_field_str_k},
            field_type_t {"person", ustore_doc_field_f32_k},
            field_type_t {"height", ustore_doc_field_i32_k},
            field_type_t {"weight", ustore_doc_field_u64_k},
        }};

        auto maybe_table = collection[{1, 2, 3, 123456, 654321}].gather(header);
        auto table = *maybe_table;
        auto col0 = table.column(0).as<std::int32_t>();
        auto col1 = table.column(1).as<value_view_t>();
        auto col2 = table.column(2).as<value_view_t>();
        auto col3 = table.column(3).as<float>();
        auto col4 = table.column(4).as<std::int32_t>();
        auto col5 = table.column(5).as<std::uint64_t>();

        EXPECT_EQ(col0[0].value, 27);
        EXPECT_EQ(col0[1].value, 27);
        EXPECT_TRUE(col0[1].converted);
        EXPECT_EQ(col0[2].value, 24);

        EXPECT_STREQ(col1[0].value.c_str(), "27");
        EXPECT_TRUE(col1[0].converted);
        EXPECT_STREQ(col1[1].value.c_str(), "27");
        EXPECT_STREQ(col1[2].value.c_str(), "24");
    }
}

#pragma region Graph Modality

edge_t make_edge(ustore_key_t edge_id, ustore_key_t v1, ustore_key_t v2) {
    return {v1, v2, edge_id};
}

std::vector<edge_t> make_edges(std::size_t vertices_count = 2, std::size_t next_connect = 1) {
    std::vector<edge_t> es;
    ustore_key_t edge_id = 0;
    for (ustore_key_t vertex_id = 0; vertex_id != vertices_count; ++vertex_id) {
        ustore_key_t connect_with = vertex_id + next_connect;
        while (connect_with < vertices_count) {
            edge_id++;
            es.push_back(make_edge(edge_id, vertex_id, connect_with));
            connect_with = connect_with + next_connect;
        }
    }
    return es;
}

/**
 * Upsert disconnected vertices into the graph.
 */
TEST(db, graph_upsert_vertices) {
    clear_environment();
    database_t db;
    EXPECT_TRUE(db.open(config().c_str()));

    graph_collection_t net = db.main<graph_collection_t>();
    std::vector<ustore_key_t> vertices {1, 4, 5, 2};
    EXPECT_TRUE(net.upsert_vertices(vertices));

    EXPECT_TRUE(*net.contains(1));
    EXPECT_TRUE(*net.contains(2));
    EXPECT_TRUE(*net.contains(4));
    EXPECT_TRUE(*net.contains(5));
}

/**
 * Upsert an edge and its member vertices into the graph.
 */
TEST(db, graph_upsert_edge) {
    clear_environment();
    database_t db;
    EXPECT_TRUE(db.open(config().c_str()));

    graph_collection_t net = db.main<graph_collection_t>();
    edge_t edge {1, 2, 9};
    EXPECT_TRUE(net.upsert_edge(edge));
    EXPECT_TRUE(*net.contains(1));
    EXPECT_TRUE(*net.contains(2));
    EXPECT_FALSE(*net.contains(3));

    auto neighbors = net.neighbors(1).throw_or_release();
    EXPECT_EQ(neighbors.size(), 1);
    EXPECT_EQ(neighbors[0], 2);
}

/**
 * Tests "Graphs" Modality, with on of the simplest network designs - a triangle.
 * Three vertices, three connections between them, forming 3 undirected, or 6 directed edges.
 * Tests edge upsert, existence checks, degree computation, vertex removals.
 */
TEST(db, graph_triangle) {

    clear_environment();
    database_t db;
    EXPECT_TRUE(db.open(config().c_str()));

    graph_collection_t net = db.main<graph_collection_t>();

    // triangle
    edge_t edge1 {1, 2, 9};
    edge_t edge2 {2, 3, 10};
    edge_t edge3 {3, 1, 11};

    EXPECT_TRUE(net.upsert_edge(edge1));
    EXPECT_TRUE(net.upsert_edge(edge2));
    EXPECT_TRUE(net.upsert_edge(edge3));

    auto neighbors = net.neighbors(1).throw_or_release();
    EXPECT_EQ(neighbors.size(), 2);
    EXPECT_EQ(neighbors[0], 2);
    EXPECT_EQ(neighbors[1], 3);

    EXPECT_TRUE(*net.contains(1));
    EXPECT_TRUE(*net.contains(2));
    EXPECT_FALSE(*net.contains(9));
    EXPECT_FALSE(*net.contains(10));
    EXPECT_FALSE(*net.contains(1000));

    EXPECT_EQ(*net.degree(1), 2u);
    EXPECT_EQ(*net.degree(2), 2u);
    EXPECT_EQ(*net.degree(3), 2u);
    EXPECT_EQ(*net.degree(1, ustore_vertex_source_k), 1u);
    EXPECT_EQ(*net.degree(2, ustore_vertex_source_k), 1u);
    EXPECT_EQ(*net.degree(3, ustore_vertex_source_k), 1u);

    EXPECT_TRUE(net.edges_containing(1));
    EXPECT_EQ(net.edges_containing(1)->size(), 2ul);
    EXPECT_EQ(net.edges_containing(1, ustore_vertex_source_k)->size(), 1ul);
    EXPECT_EQ(net.edges_containing(1, ustore_vertex_target_k)->size(), 1ul);

    EXPECT_EQ(net.edges_containing(3, ustore_vertex_target_k)->size(), 1ul);
    EXPECT_EQ(net.edges_containing(2, ustore_vertex_source_k)->size(), 1ul);
    EXPECT_EQ((*net.edges_containing(3, ustore_vertex_target_k))[0].source_id, 2);
    EXPECT_EQ((*net.edges_containing(3, ustore_vertex_target_k))[0].target_id, 3);
    EXPECT_EQ((*net.edges_containing(3, ustore_vertex_target_k))[0].id, 10);
    EXPECT_EQ(net.edges_between(3, 1)->size(), 1ul);
    EXPECT_EQ(net.edges_between(1, 3)->size(), 0ul);

    // Check scans
    EXPECT_TRUE(net.edges());
    {
        std::vector<edge_t> expected_edges {edge1, edge2, edge3};
        std::vector<edge_t> exported_edges;

        auto present_edges = *net.edges(ustore_vertex_source_k);
        auto present_it = std::move(present_edges).begin();
        auto count_results = 0;
        while (!present_it.is_end()) {
            exported_edges.push_back(*present_it);
            ++present_it;
            ++count_results;
        }
        EXPECT_EQ(count_results, 3);
        EXPECT_EQ(exported_edges, expected_edges);
    }

    // Remove a single edge, making sure that the nodes info persists
    EXPECT_TRUE(net.remove_edges({
        {{&edge1.source_id}, 1},
        {{&edge1.target_id}, 1},
        {{&edge1.id}, 1},
    }));
    EXPECT_TRUE(*net.contains(1));
    EXPECT_TRUE(*net.contains(2));
    EXPECT_EQ(net.edges_between(1, 2)->size(), 0ul);

    // Bring that edge back
    EXPECT_TRUE(net.upsert_edges({
        {{&edge1.source_id}, 1},
        {{&edge1.target_id}, 1},
        {{&edge1.id}, 1},
    }));
    EXPECT_EQ(net.edges_between(1, 2)->size(), 1ul);

    // Remove a vertex
    ustore_key_t vertex_to_remove = 2;
    EXPECT_TRUE(net.remove_vertex(vertex_to_remove));
    EXPECT_FALSE(*net.contains(vertex_to_remove));
    EXPECT_EQ(net.edges_containing(vertex_to_remove)->size(), 0ul);
    EXPECT_EQ(net.edges_between(1, vertex_to_remove)->size(), 0ul);
    EXPECT_EQ(net.edges_between(vertex_to_remove, 1)->size(), 0ul);

    // Bring back the whole graph
    EXPECT_TRUE(net.upsert_edge(edge1));
    EXPECT_TRUE(net.upsert_edge(edge2));
    EXPECT_TRUE(net.upsert_edge(edge3));
    EXPECT_TRUE(*net.contains(vertex_to_remove));
    EXPECT_EQ(net.edges_containing(vertex_to_remove)->size(), 2ul);
    EXPECT_EQ(net.edges_between(1, vertex_to_remove)->size(), 1ul);
    EXPECT_EQ(net.edges_between(vertex_to_remove, 1)->size(), 0ul);
}

/**
 * Further complicates the `graph_triangle` test by performing all of the updates
 * and lookups in batches. This detects inconsistencies in concurrent updates to
 * the underlying binary representation, triggered from a single high-level
 * graph operation.
 */
TEST(db, graph_triangle_batch) {

    clear_environment();
    database_t db;
    EXPECT_TRUE(db.open(config().c_str()));

    blobs_collection_t main = db.main();
    graph_collection_t net = db.main<graph_collection_t>();

    std::vector<edge_t> triangle {
        {1, 2, 9},
        {2, 3, 10},
        {3, 1, 11},
    };

    EXPECT_TRUE(net.upsert_edges(edges(triangle)));
    EXPECT_TRUE(*net.contains(1));
    EXPECT_TRUE(*net.contains(2));
    EXPECT_FALSE(*net.contains(9));
    EXPECT_FALSE(*net.contains(10));
    EXPECT_FALSE(*net.contains(1000));

    EXPECT_EQ(*net.degree(1), 2u);
    EXPECT_EQ(*net.degree(2), 2u);
    EXPECT_EQ(*net.degree(3), 2u);
    EXPECT_EQ(*net.degree(1, ustore_vertex_source_k), 1u);
    EXPECT_EQ(*net.degree(2, ustore_vertex_source_k), 1u);
    EXPECT_EQ(*net.degree(3, ustore_vertex_source_k), 1u);

    EXPECT_TRUE(net.edges_containing(1));
    EXPECT_EQ(net.edges_containing(1)->size(), 2ul);
    EXPECT_EQ(net.edges_containing(1, ustore_vertex_source_k)->size(), 1ul);
    EXPECT_EQ(net.edges_containing(1, ustore_vertex_target_k)->size(), 1ul);

    EXPECT_EQ(net.edges_containing(3, ustore_vertex_target_k)->size(), 1ul);
    EXPECT_EQ(net.edges_containing(2, ustore_vertex_source_k)->size(), 1ul);
    EXPECT_EQ((*net.edges_containing(3, ustore_vertex_target_k))[0].source_id, 2);
    EXPECT_EQ((*net.edges_containing(3, ustore_vertex_target_k))[0].target_id, 3);
    EXPECT_EQ((*net.edges_containing(3, ustore_vertex_target_k))[0].id, 10);
    EXPECT_EQ(net.edges_between(3, 1)->size(), 1ul);
    EXPECT_EQ(net.edges_between(1, 3)->size(), 0ul);

    // Check scans
    EXPECT_TRUE(net.edges());
    {
        std::vector<edge_t> expected_edges {triangle.begin(), triangle.end()};
        std::vector<edge_t> exported_edges;

        auto present_edges = *net.edges(ustore_vertex_source_k);
        auto present_it = std::move(present_edges).begin();
        size_t count_results = 0ul;
        while (!present_it.is_end()) {
            exported_edges.push_back(*present_it);
            ++present_it;
            ++count_results;
        }
        EXPECT_EQ(count_results, triangle.size());
        EXPECT_EQ(exported_edges, expected_edges);
    }

    // Remove a single edge, making sure that the nodes info persists
    EXPECT_TRUE(net.remove_edges(edges_view_t {
        {{&triangle[0].source_id}, 1},
        {{&triangle[0].target_id}, 1},
        {{&triangle[0].id}, 1},
    }));
    EXPECT_TRUE(*net.contains(1));
    EXPECT_TRUE(*net.contains(2));
    EXPECT_EQ(net.edges_between(1, 2)->size(), 0ul);

    // Bring that edge back
    EXPECT_TRUE(net.upsert_edges(edges_view_t {
        {{&triangle[0].source_id}, 1},
        {{&triangle[0].target_id}, 1},
        {{&triangle[0].id}, 1},
    }));
    EXPECT_EQ(net.edges_between(1, 2)->size(), 1ul);

    // Remove a vertex
    ustore_key_t vertex_to_remove = 2;
    EXPECT_TRUE(net.remove_vertex(vertex_to_remove));
    EXPECT_FALSE(*net.contains(vertex_to_remove));
    EXPECT_EQ(net.edges_containing(vertex_to_remove)->size(), 0ul);
    EXPECT_EQ(net.edges_between(1, vertex_to_remove)->size(), 0ul);
    EXPECT_EQ(net.edges_between(vertex_to_remove, 1)->size(), 0ul);

    // Bring back the whole graph
    EXPECT_TRUE(net.upsert_edges(edges(triangle)));
    EXPECT_TRUE(*net.contains(vertex_to_remove));
    EXPECT_EQ(net.edges_containing(vertex_to_remove)->size(), 2ul);
    EXPECT_EQ(net.edges_between(1, vertex_to_remove)->size(), 1ul);
    EXPECT_EQ(net.edges_between(vertex_to_remove, 1)->size(), 0ul);
}

/**
 * Tries to make a transaction on a graph, that must fail to `commit`.
 * Creates a "wedge": A-B-C. If a transaction changes the B-C edge,
 * while A-B is updated externally, the commit will fail.
 */
TEST(db, graph_transaction_watch) {
    if (!ustore_supports_transactions_k)
        return;

    clear_environment();
    database_t db;
    EXPECT_TRUE(db.open(config().c_str()));
    graph_collection_t net = db.main<graph_collection_t>();

    edge_t edge_ab {'A', 'B', 19};
    edge_t edge_bc {'B', 'C', 31};
    EXPECT_TRUE(net.upsert_edge(edge_ab));
    EXPECT_TRUE(net.upsert_edge(edge_bc));

    transaction_t txn = *db.transact();
    graph_collection_t txn_net = txn.main<graph_collection_t>();
    EXPECT_EQ(txn_net.degree('B'), 2);
    EXPECT_TRUE(txn_net.remove_edge(edge_bc));
    EXPECT_TRUE(net.remove_edge(edge_ab));

    EXPECT_FALSE(txn.commit());
}

/**
 * Constructs a larger graph, validating the degrees in a resulting network afterward.
 */
TEST(db, graph_random_fill) {
    clear_environment();
    database_t db;
    EXPECT_TRUE(db.open(config().c_str()));

    graph_collection_t graph = db.main<graph_collection_t>();

    constexpr std::size_t vertices_count = 1000;
    auto edges_vec = make_edges(vertices_count, 100);
    EXPECT_TRUE(graph.upsert_edges(edges(edges_vec)));

    for (ustore_key_t vertex_id = 0; vertex_id != vertices_count; ++vertex_id) {
        EXPECT_TRUE(graph.contains(vertex_id));
        EXPECT_EQ(*graph.degree(vertex_id), 9u);
    }
}

/**
 * Inserts two edges with a shared vertex in two separate transactions.
 * The latter insert must fail, as it depends on the preceding state of the vertex.
 */
TEST(db, graph_conflicting_transactions) {
    if (!ustore_supports_transactions_k)
        return;

    clear_environment();
    database_t db;
    EXPECT_TRUE(db.open(config().c_str()));

    transaction_t txn = *db.transact();
    graph_collection_t txn_net = txn.main<graph_collection_t>();
    transaction_t txn2 = *db.transact();
    graph_collection_t txn_net2 = txn2.main<graph_collection_t>();

    edge_t edge4 {4, 5, 15};
    edge_t edge5 {5, 6, 16};

    EXPECT_TRUE(txn_net.upsert_edge(edge4));
    EXPECT_TRUE(txn_net2.upsert_edge(edge5));
    EXPECT_TRUE(txn.commit());
    EXPECT_FALSE(txn2.commit());
}

/**
 * Takes a single Graph Store and populates it with various 5-vertex shapes:
 * a star, a pentagon, and five self-loops.
 */
TEST(db, graph_layering_shapes) {
    clear_environment();
    database_t db;
    EXPECT_TRUE(db.open(config().c_str()));

    graph_collection_t graph = db.main<graph_collection_t>();

    std::vector<ustore_key_t> vertices = {1, 2, 3, 4, 5};
    auto over_the_vertices = [&](bool exist, size_t degree) {
        for (auto& vertex_id : vertices) {
            EXPECT_EQ(*graph.contains(vertex_id), exist);
            EXPECT_EQ(*graph.degree(vertex_id), degree);
        }
    };

    // Before insertions, the graph is empty.
    over_the_vertices(false, 0);

    std::vector<edge_t> star {
        {1, 3, 1},
        {1, 4, 2},
        {2, 4, 3},
        {2, 5, 4},
        {3, 5, 5},
    };
    std::vector<edge_t> pentagon {
        {1, 2, 6},
        {2, 3, 7},
        {3, 4, 8},
        {4, 5, 9},
        {5, 1, 10},
    };
    std::vector<edge_t> self_loops {
        {1, 1, 11},
        {2, 2, 12},
        {3, 3, 13},
        {4, 4, 14},
        {5, 5, 15},
    };

    EXPECT_TRUE(graph.upsert_edges(edges(star)));
    over_the_vertices(true, 2u);
    EXPECT_TRUE(graph.upsert_edges(edges(pentagon)));
    over_the_vertices(true, 4u);
    EXPECT_TRUE(graph.remove_edges(edges(star)));
    over_the_vertices(true, 2u);
    EXPECT_TRUE(graph.upsert_edges(edges(star)));
    over_the_vertices(true, 4u);
    EXPECT_TRUE(graph.remove_edges(edges(pentagon)));
    over_the_vertices(true, 2u);
    EXPECT_TRUE(graph.upsert_edges(edges(pentagon)));
    over_the_vertices(true, 4u);
    EXPECT_TRUE(graph.upsert_edges(edges(self_loops)));
    over_the_vertices(true, 6u);
    EXPECT_TRUE(graph.remove_edges(edges(star)));
    EXPECT_TRUE(graph.remove_edges(edges(pentagon)));
    over_the_vertices(true, 2u);
    EXPECT_TRUE(graph.remove_edges(edges(self_loops)));
    over_the_vertices(true, 0);
    EXPECT_TRUE(db.clear());
    over_the_vertices(false, 0);
}

/**
 * Tests vertex removals, which are the hardest operations on Graphs,
 * as they trigger updates in all nodes connected to the removed one.
 */
TEST(db, graph_remove_vertices) {
    clear_environment();
    database_t db;
    EXPECT_TRUE(db.open(config().c_str()));

    graph_collection_t graph = db.main<graph_collection_t>();

    constexpr std::size_t vertices_count = 1000;
    auto edges_vec = make_edges(vertices_count, 100);
    EXPECT_TRUE(graph.upsert_edges(edges(edges_vec)));

    for (ustore_key_t vertex_id = 0; vertex_id != vertices_count; ++vertex_id) {
        EXPECT_TRUE(graph.contains(vertex_id));
        EXPECT_TRUE(*graph.contains(vertex_id));
        EXPECT_TRUE(graph.remove_vertex(vertex_id));
        EXPECT_TRUE(graph.contains(vertex_id));
        EXPECT_FALSE(*graph.contains(vertex_id));
    }

    EXPECT_TRUE(graph.upsert_edges(edges(edges_vec)));
    std::vector<ustore_key_t> vertices(vertices_count);
    std::iota(vertices.begin(), vertices.end(), 0);
    EXPECT_TRUE(graph.remove_vertices(vertices));
    for (ustore_key_t vertex_id = 0; vertex_id != vertices_count; ++vertex_id) {
        EXPECT_TRUE(graph.contains(vertex_id));
        EXPECT_FALSE(*graph.contains(vertex_id));
    }
}

/**
 * Removes just the known list of edges, checking that vertices remain
 * in the graph, even though entirely disconnected.
 */
TEST(db, graph_remove_edges_keep_vertices) {
    clear_environment();
    database_t db;
    EXPECT_TRUE(db.open(config().c_str()));

    graph_collection_t graph = db.main<graph_collection_t>();

    constexpr std::size_t vertices_count = 1000;
    auto edges_vec = make_edges(vertices_count, 100);
    EXPECT_TRUE(graph.upsert_edges(edges(edges_vec)));
    EXPECT_TRUE(graph.remove_edges(edges(edges_vec)));

    for (ustore_key_t vertex_id = 0; vertex_id != vertices_count; ++vertex_id) {
        EXPECT_TRUE(graph.contains(vertex_id));
        EXPECT_TRUE(*graph.contains(vertex_id));
    }
}
/**
 * Add edges to the graph. Checks how many edges each vertex has.
 * Then remove the edges, making sure that the vertices aren't
 * connected to each other anymore.
 */
TEST(db, graph_get_vertex_edges) {
    clear_environment();
    database_t db;
    EXPECT_TRUE(db.open(config().c_str()));

    graph_collection_t graph = db.main<graph_collection_t>();

    constexpr std::size_t vertices_count = 1000;
    auto edges_vec = make_edges(vertices_count, 100);
    EXPECT_TRUE(graph.upsert_edges(edges(edges_vec)));

    std::vector<edge_t> received_edges;
    for (ustore_key_t vertex_id = 0; vertex_id != vertices_count; ++vertex_id) {
        auto es = *graph.edges_containing(vertex_id);
        EXPECT_EQ(es.size(), 9u);
        for (size_t i = 0; i != es.size(); ++i)
            received_edges.push_back(es[i]);
    }
    EXPECT_TRUE(graph.remove_edges(edges(received_edges)));

    for (ustore_key_t vertex_id = 0; vertex_id != vertices_count; ++vertex_id) {
        EXPECT_TRUE(graph.contains(vertex_id));
        EXPECT_TRUE(*graph.contains(vertex_id));
        EXPECT_EQ(graph.edges_containing(vertex_id)->size(), 0);
    }
}

/**
 * Getting the degrees of multiple vertices simultaneously.
 */
TEST(db, graph_degrees) {
    clear_environment();
    database_t db;
    EXPECT_TRUE(db.open(config().c_str()));

    graph_collection_t graph = db.main<graph_collection_t>();

    constexpr std::size_t vertices_count = 1000;
    std::vector<ustore_key_t> vertices(vertices_count);
    std::iota(vertices.begin(), vertices.end(), 0);

    auto edges_vec = make_edges(vertices_count, 100);
    EXPECT_TRUE(graph.upsert_edges(edges(edges_vec)));

    auto degrees = *graph.degrees(strided_range(vertices).immutable());
    EXPECT_EQ(degrees.size(), vertices_count);
}

TEST(db, graph_neighbors) {
    clear_environment();
    database_t db;
    EXPECT_TRUE(db.open(config().c_str()));

    graph_collection_t graph = db.main<graph_collection_t>();
    edge_t edge1 {1, 1, 17};
    edge_t edge2 {1, 2, 15};
    edge_t edge3 {2, 3, 16};

    EXPECT_TRUE(graph.upsert_edge(edge1));
    EXPECT_TRUE(graph.upsert_edge(edge2));
    EXPECT_TRUE(graph.upsert_edge(edge3));

    auto neighbors = graph.neighbors(1).throw_or_release();
    EXPECT_EQ(neighbors.size(), 2);
    EXPECT_EQ(neighbors[0], 1);
    EXPECT_EQ(neighbors[1], 2);

    neighbors = graph.neighbors(2).throw_or_release();
    EXPECT_EQ(neighbors.size(), 2);
    EXPECT_EQ(neighbors[0], 1);
    EXPECT_EQ(neighbors[1], 3);
}

#pragma region Vectors Modality

/**
 * Tests "Vector Modality", including both CRUD and more analytical approximate search
 * operations with just three distinctly different vectors in R3 space with Cosine metric.
 */
TEST(db, vectors) {
    clear_environment();
    database_t db;
    EXPECT_TRUE(db.open(config().c_str()));

    constexpr std::size_t dims_k = 3;
    ustore_key_t keys[3] = {'a', 'b', 'c'};
    float vectors[3][dims_k] = {
        {0.3, 0.1, 0.2},
        {0.35, 0.1, 0.2},
        {-0.1, 0.2, 0.5},
    };

    arena_t arena(db);
    status_t status;

    float* vector_first_begin = &vectors[0][0];
    ustore_vectors_write_t write {};
    write.db = db;
    write.arena = arena.member_ptr();
    write.error = status.member_ptr();
    write.dimensions = dims_k;
    write.keys = keys;
    write.keys_stride = sizeof(ustore_key_t);
    write.vectors_starts = (ustore_bytes_cptr_t*)&vector_first_begin;
    write.vectors_stride = sizeof(float) * dims_k;
    write.tasks_count = 3;
    ustore_vectors_write(&write);
    EXPECT_TRUE(status);

    ustore_length_t max_results = 2;
    ustore_length_t* found_results = nullptr;
    ustore_key_t* found_keys = nullptr;
    ustore_float_t* found_distances = nullptr;
    ustore_vectors_search_t search {};
    search.db = db;
    search.arena = arena.member_ptr();
    search.error = status.member_ptr();
    search.dimensions = dims_k;
    search.tasks_count = 1;
    search.match_counts_limits = &max_results;
    search.queries_starts = (ustore_bytes_cptr_t*)&vector_first_begin;
    search.queries_stride = sizeof(float) * dims_k;
    search.match_counts = &found_results;
    search.match_keys = &found_keys;
    search.match_metrics = &found_distances;
    search.metric = ustore_vector_metric_cos_k;
    ustore_vectors_search(&search);
    EXPECT_TRUE(status);

    EXPECT_EQ(found_results[0], max_results);
    EXPECT_EQ(found_keys[0], ustore_key_t('a'));
    EXPECT_EQ(found_keys[1], ustore_key_t('b'));
}

int main(int argc, char** argv) {

#if defined(USTORE_FLIGHT_CLIENT)
    srv_path = argv[0];
    srv_path = srv_path.substr(0, srv_path.find_last_of("/") + 1) + "ustore_flight_server_ucset";
#endif

    auto directory_str = path() ? std::string_view(path()) : "";
    if (directory_str.size())
        std::printf("Will work in directory: %s\n", directory_str.data());
    else
        std::printf("Will work with default configuration\n");

    ::testing::InitGoogleTest(&argc, argv);
    int status = RUN_ALL_TESTS();
#if defined(USTORE_FLIGHT_CLIENT)
    kill(srv_id, SIGKILL);
    waitpid(srv_id, nullptr, 0);
#endif
    return status;
}
