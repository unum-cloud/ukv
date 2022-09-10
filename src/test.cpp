/**
 * @file test.cpp
 * @author Ashot Vardanian
 * @date 2022-07-06
 *
 * @brief A set of tests implemented using Google Test.
 */

#include <unordered_set>
#include <vector>
#include <filesystem>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "ukv/ukv.hpp"

using namespace unum::ukv;
using namespace unum;

#define macro_concat_(prefix, suffix) prefix##suffix
#define macro_concat(prefix, suffix) macro_concat_(prefix, suffix)
#define _ [[maybe_unused]] auto macro_concat(_, __LINE__)
#define M_EXPECT_EQ_JSON(str1, str2) EXPECT_EQ(json_t::parse((str1)), json_t::parse((str2)));
#define M_EXPECT_EQ_MSG(str1, str2) \
    EXPECT_EQ(json_t::from_msgpack((str1).c_str(), (str1).c_str() + (str1).size()), json_t::parse((str2)));
#if 0
TEST(db, intro) {

    database_t db;
    EXPECT_TRUE(db.open());

    // Try getting the main collection
    EXPECT_TRUE(db.collection());
    collection_t main = *db.collection();

    // Single-element access
    main[42] = "purpose of life";
    main.at(42) = "purpose of life";
    EXPECT_EQ(*main[42].value(), "purpose of life");
    _ = main[42].clear();

    // Mapping multiple keys to same values
    main[{43, 44}] = "same value";

    // Operations on smart-references
    _ = main[{43, 44}].clear();
    _ = main[{43, 44}].erase();
    _ = main[{43, 44}].present();
    _ = main[{43, 44}].length();
    _ = main[{43, 44}].value();
    _ = main[std::array<ukv_key_t, 3> {65, 66, 67}];
    _ = main[std::vector<ukv_key_t> {65, 66, 67, 68}];
    // for (value_view_t value : *main[{100, 101}].value())
    //     (void)value;

    // Accessing named collections
    collection_t prefixes = *db.collection("prefixes");
    prefixes.at(42) = "purpose";
    db["articles"]->at(42) = "of";
    db["suffixes"]->at(42) = "life";

    // Reusable memory
    // This interface not just more performant, but also provides nicer interface:
    //  expected_gt<joined_bins_t> tapes = main[{100, 101}].on(arena);
    arena_t arena(db);
    _ = main[{43, 44}].on(arena).clear();
    _ = main[{43, 44}].on(arena).erase();
    _ = main[{43, 44}].on(arena).present();
    _ = main[{43, 44}].on(arena).length();
    _ = main[{43, 44}].on(arena).value();

    // Batch-assignment: many keys to many values
    // main[std::array<ukv_key_t, 3> {65, 66, 67}] = std::array {"A", "B", "C"};
    // main[std::array {ckf(prefixes, 65), ckf(66), ckf(67)}] = std::array {"A", "B", "C"};

    // Iterating over collections
    for (ukv_key_t key : main.keys())
        (void)key;
    for (ukv_key_t key : main.keys(100, 200))
        (void)key;

    _ = main.members(100, 200).size_estimates()->cardinality;

    // Supporting options
    _ = main[{43, 44}].on(arena).clear(/*flush:*/ false);
    _ = main[{43, 44}].on(arena).erase(/*flush:*/ false);
    _ = main[{43, 44}].on(arena).present(/*track:*/ false);
    _ = main[{43, 44}].on(arena).length(/*track:*/ false);
    _ = main[{43, 44}].on(arena).value(/*track:*/ false);

    // Working with sub documents
    main[56] = R"( {"hello": "world", "answer": 42} )"_json.dump().c_str();
    _ = main[ckf(56, "hello")].value() == "world";

    EXPECT_TRUE(db.clear());
}
#endif
#pragma region Binary Collections

template <typename locations_at>
void check_length(members_ref_gt<locations_at>& ref, ukv_length_t expected_length) {

    EXPECT_TRUE(ref.value()) << "Failed to fetch missing keys";

    auto const expects_missing = expected_length == ukv_length_missing_k;
    using extractor_t = places_arg_extractor_gt<locations_at>;
    ukv_size_t count = extractor_t {}.count(ref.locations());

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

template <typename locations_at>
void check_equalities(members_ref_gt<locations_at>& ref, contents_arg_t values) {

    EXPECT_TRUE(ref.value()) << "Failed to fetch present keys";
    using extractor_t = places_arg_extractor_gt<locations_at>;

    // Validate that values match
    auto maybe_retrieved = ref.value();
    auto const& retrieved = *maybe_retrieved;
    EXPECT_EQ(retrieved.size(), extractor_t {}.count(ref.locations()));

    auto it = retrieved.begin();
    for (std::size_t i = 0; i != extractor_t {}.count(ref.locations()); ++i, ++it) {
        auto expected_len = static_cast<std::size_t>(values.lengths_begin[i]);
        auto expected_begin = reinterpret_cast<byte_t const*>(values.contents_begin[i]) + values.offsets_begin[i];

        value_view_t val_view = *it;
        value_view_t expected_view(expected_begin, expected_begin + expected_len);
        EXPECT_EQ(val_view.size(), expected_len);
        EXPECT_EQ(val_view, expected_view);
    }
}

template <typename locations_at>
void round_trip(members_ref_gt<locations_at>& ref, contents_arg_t values) {
    EXPECT_TRUE(ref.assign(values)) << "Failed to assign";
    check_equalities(ref, values);
}

void check_binary_collection(collection_t& col) {
    std::vector<ukv_key_t> keys {34, 35, 36};
    ukv_length_t val_len = sizeof(std::uint64_t);
    std::vector<std::uint64_t> vals {34, 35, 36};
    std::vector<ukv_length_t> offs {0, val_len, val_len * 2};
    auto vals_begin = reinterpret_cast<ukv_bytes_ptr_t>(vals.data());

    auto ref = col[keys];
    contents_arg_t values {
        .contents_begin = {&vals_begin, 0},
        .offsets_begin = {offs.data(), sizeof(ukv_length_t)},
        .lengths_begin = {&val_len, 0},
        .count = 3,
    };
    round_trip(ref, values);

    // Overwrite those values with same size integers and try again
    for (auto& val : vals)
        val += 100;
    round_trip(ref, values);

    // Overwrite with empty values, but check for existence
    EXPECT_TRUE(ref.clear());
    check_length(ref, 0);

    // Check scans
    keys_range_t present_keys = col.keys();
    keys_stream_t present_it = present_keys.begin();
    auto expected_it = keys.begin();
    for (; expected_it != keys.end(); ++present_it, ++expected_it) {
        EXPECT_EQ(*expected_it, *present_it);
    }
    EXPECT_TRUE(present_it.is_end());

    // Remove all of the values and check that they are missing
    EXPECT_TRUE(ref.erase());
    check_length(ref, ukv_length_missing_k);
}

TEST(db, basic) {

    database_t db;
    EXPECT_TRUE(db.open(""));

    // Try getting the main collection
    EXPECT_TRUE(db.collection());
    collection_t col = *db.collection();
    check_binary_collection(col);
    EXPECT_TRUE(db.clear());
}

TEST(db, named) {
    database_t db;
    EXPECT_TRUE(db.open(""));

    EXPECT_TRUE(db["col1"]);
    EXPECT_TRUE(db["col2"]);

    collection_t col1 = *(db["col1"]);
    collection_t col2 = *(db["col2"]);

    EXPECT_TRUE(*db.contains("col1"));
    EXPECT_TRUE(*db.contains("col2"));
    EXPECT_FALSE(*db.contains("unknown_col"));

    check_binary_collection(col1);
    check_binary_collection(col2);

    EXPECT_TRUE(db.remove("col1"));
    EXPECT_TRUE(db.remove("col2"));
    EXPECT_FALSE(*db.contains("col1"));
    EXPECT_FALSE(*db.contains("col2"));
    EXPECT_TRUE(db.clear());
}

TEST(db, txn) {
    database_t db;
    EXPECT_TRUE(db.open(""));
    EXPECT_TRUE(db.transact());
    transaction_t txn = *db.transact();

    std::vector<ukv_key_t> keys {54, 55, 56};
    ukv_length_t val_len = sizeof(std::uint64_t);
    std::vector<std::uint64_t> vals {54, 55, 56};
    std::vector<ukv_length_t> offs {0, val_len, val_len * 2};
    auto vals_begin = reinterpret_cast<ukv_bytes_ptr_t>(vals.data());

    contents_arg_t values {
        .contents_begin = {&vals_begin, 0},
        .offsets_begin = {offs.data(), sizeof(ukv_length_t)},
        .lengths_begin = {&val_len, 0},
        .count = 3,
    };

    auto txn_ref = txn[keys];
    round_trip(txn_ref, values);

    EXPECT_TRUE(db.collection());
    collection_t col = *db.collection();
    auto col_ref = col[keys];

    // Check for missing values before commit
    check_length(col_ref, ukv_length_missing_k);

    auto status = txn.commit();
    status.throw_unhandled();
    status = txn.reset();
    status.throw_unhandled();

    // Validate that values match after commit
    check_equalities(col_ref, values);

    // Transaction with named collection
    EXPECT_TRUE(db.collection("named_col"));
    collection_t named_col = *db.collection("named_col");
    std::vector<col_key_t> sub_keys {{named_col, 54}, {named_col, 55}, {named_col, 56}};
    auto txn_named_col_ref = txn[sub_keys];
    round_trip(txn_named_col_ref, values);

    // Check for missing values before commit
    auto named_col_ref = named_col[keys];
    check_length(named_col_ref, ukv_length_missing_k);

    status = txn.commit();
    status.throw_unhandled();
    status = txn.reset();
    status.throw_unhandled();

    // Validate that values match after commit
    check_equalities(named_col_ref, values);
    EXPECT_TRUE(db.clear());
}

#pragma region Document Collections

TEST(db, docs) {
    using json_t = nlohmann::json;
    database_t db;
    EXPECT_TRUE(db.open(""));

    // JSON
    collection_t col = *db.collection("docs", ukv_format_json_k);
    auto json = R"( {"person": "Davit", "age": 24} )"_json.dump();
    col[1] = json.c_str();
    M_EXPECT_EQ_JSON(col[1].value()->c_str(), json.c_str());
    M_EXPECT_EQ_JSON(col[ckf(1, "person")].value()->c_str(), "\"Davit\"");
    M_EXPECT_EQ_JSON(col[ckf(1, "age")].value()->c_str(), "24");

    // MsgPack
    col.as(ukv_format_msgpack_k);
    value_view_t val = *col[1].value();
    M_EXPECT_EQ_MSG(val, json.c_str());
    val = *col[ckf(1, "person")].value();
    M_EXPECT_EQ_MSG(val, "\"Davit\"");
    val = *col[ckf(1, "age")].value();
    M_EXPECT_EQ_MSG(val, "24");

    // Binary
    col.as(ukv_format_binary_k);
    auto maybe_person = col[ckf(1, "person")].value();
    EXPECT_EQ(std::string_view(maybe_person->c_str(), maybe_person->size()), std::string_view("Davit"));

    // JSON-Patching
    col.as(ukv_format_json_patch_k);
    auto json_patch =
        R"( [
            { "op": "replace", "path": "/person", "value": "Ashot" },
            { "op": "add", "path": "/hello", "value": ["world"] },
            { "op": "remove", "path": "/age" }
            ] )"_json.dump();
    auto expected_json = R"( {"person": "Ashot", "hello": ["world"]} )"_json.dump();
    col[1] = json_patch.c_str();
    auto patch_result = col[1].value();
    M_EXPECT_EQ_JSON(patch_result->c_str(), expected_json.c_str());
    M_EXPECT_EQ_JSON(col[ckf(1, "person")].value()->c_str(), "\"Ashot\"");
    M_EXPECT_EQ_JSON(col[ckf(1, "/hello/0")].value()->c_str(), "\"world\"");

    // JSON-Patch Merging
    col.as(ukv_format_json_merge_patch_k);
    auto json_to_merge = R"( {"person": "Darvin", "age": 28} )"_json.dump();
    expected_json = R"( {"person": "Darvin", "hello": ["world"], "age": 28} )"_json.dump();
    col[1] = json_to_merge.c_str();
    auto merge_result = col[1].value();
    M_EXPECT_EQ_JSON(merge_result->c_str(), expected_json.c_str());
    M_EXPECT_EQ_JSON(col[ckf(1, "person")].value()->c_str(), "\"Darvin\"");
    M_EXPECT_EQ_JSON(col[ckf(1, "/hello/0")].value()->c_str(), "\"world\"");
    M_EXPECT_EQ_JSON(col[ckf(1, "age")].value()->c_str(), "28");
    EXPECT_TRUE(db.clear());
}

TEST(db, docs_table) {
    using json_t = nlohmann::json;
    database_t db;
    EXPECT_TRUE(db.open(""));

    // Inject basic data
    collection_t col = *db.collection("", ukv_format_json_k);
    auto json_ashot = R"( { "person": "Ashot", "age": 27, "height": 1 } )"_json.dump();
    auto json_darvin = R"( { "person": "Darvin", "age": "27", "weight": 2 } )"_json.dump();
    auto json_davit = R"( { "person": "Davit", "age": 24 } )"_json.dump();
    col[1] = json_ashot.c_str();
    col[2] = json_darvin.c_str();
    col[3] = json_davit.c_str();
    M_EXPECT_EQ_JSON(*col[1].value(), json_ashot.c_str());
    M_EXPECT_EQ_JSON(*col[2].value(), json_darvin.c_str());

    // Just column names
    {
        auto maybe_fields = col[1].gist();
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
        auto maybe_table = col[1].gather(header);
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

        auto maybe_table = col[1].gather(header);
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
        auto maybe_table = col[{1, 2, 3, 123456}].gather(header);
        auto table = *maybe_table;
        auto col0 = table.column<0>();

        EXPECT_EQ(col0[0].value, 27);
        EXPECT_EQ(col0[1].value, 27);
        EXPECT_TRUE(col0[1].converted);
        EXPECT_EQ(col0[2].value, 24);
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

        auto maybe_table = col[{1, 2, 3, 123456, 654321}].gather(header);
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
            field_type_t {"age", ukv_type_i32_k},
            field_type_t {"age", ukv_type_str_k},
            field_type_t {"person", ukv_type_str_k},
            field_type_t {"person", ukv_type_f32_k},
            field_type_t {"height", ukv_type_i32_k},
            field_type_t {"weight", ukv_type_u64_k},
        }};

        auto maybe_table = col[{1, 2, 3, 123456, 654321}].gather(header);
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

    EXPECT_TRUE(db.clear());
}

#pragma region Graph Collections

TEST(db, graph_triangle) {

    database_t db;
    EXPECT_TRUE(db.open(""));

    collection_t main = *db.collection();
    graph_ref_t net = main.as_graph();

    // triangle
    edge_t edge1 {1, 2, 9};
    edge_t edge2 {2, 3, 10};
    edge_t edge3 {3, 1, 11};

    EXPECT_TRUE(net.upsert(edge1));
    EXPECT_TRUE(net.upsert(edge2));
    EXPECT_TRUE(net.upsert(edge3));

    EXPECT_TRUE(*net.contains(1));
    EXPECT_TRUE(*net.contains(2));
    EXPECT_FALSE(*net.contains(9));
    EXPECT_FALSE(*net.contains(10));
    EXPECT_FALSE(*net.contains(1000));

    EXPECT_EQ(*net.degree(1), 2u);
    EXPECT_EQ(*net.degree(2), 2u);
    EXPECT_EQ(*net.degree(3), 2u);
    EXPECT_EQ(*net.degree(1, ukv_vertex_source_k), 1u);
    EXPECT_EQ(*net.degree(2, ukv_vertex_source_k), 1u);
    EXPECT_EQ(*net.degree(3, ukv_vertex_source_k), 1u);

    EXPECT_TRUE(net.edges(1));
    EXPECT_EQ(net.edges(1)->size(), 2ul);
    EXPECT_EQ(net.edges(1, ukv_vertex_source_k)->size(), 1ul);
    EXPECT_EQ(net.edges(1, ukv_vertex_target_k)->size(), 1ul);

    EXPECT_EQ(net.edges(3, ukv_vertex_target_k)->size(), 1ul);
    EXPECT_EQ(net.edges(2, ukv_vertex_source_k)->size(), 1ul);
    EXPECT_EQ((*net.edges(3, ukv_vertex_target_k))[0].source_id, 2);
    EXPECT_EQ((*net.edges(3, ukv_vertex_target_k))[0].target_id, 3);
    EXPECT_EQ((*net.edges(3, ukv_vertex_target_k))[0].id, 10);
    EXPECT_EQ(net.edges(3, 1)->size(), 1ul);
    EXPECT_EQ(net.edges(1, 3)->size(), 0ul);

    // Check scans
    EXPECT_TRUE(net.edges());
    {
        std::unordered_set<edge_t, edge_hash_t> expected_edges {edge1, edge2, edge3};
        std::unordered_set<edge_t, edge_hash_t> exported_edges;

        auto present_edges = *net.edges();
        auto present_it = std::move(present_edges).begin();
        auto count_results = 0;
        while (!present_it.is_end()) {
            exported_edges.insert(*present_it);
            ++present_it;
            ++count_results;
        }
        EXPECT_EQ(count_results, 6);
        EXPECT_EQ(exported_edges, expected_edges);
    }

    // Remove a single edge, making sure that the nodes info persists
    EXPECT_TRUE(net.remove({
        {&edge1.source_id},
        {&edge1.target_id},
        {&edge1.id},
    }));
    EXPECT_TRUE(*net.contains(1));
    EXPECT_TRUE(*net.contains(2));
    EXPECT_EQ(net.edges(1, 2)->size(), 0ul);

    // Bring that edge back
    EXPECT_TRUE(net.upsert({
        {&edge1.source_id},
        {&edge1.target_id},
        {&edge1.id},
    }));
    EXPECT_EQ(net.edges(1, 2)->size(), 1ul);

    // Remove a vertex
    ukv_key_t vertex_to_remove = 2;
    EXPECT_TRUE(net.remove(vertex_to_remove));
    EXPECT_FALSE(*net.contains(vertex_to_remove));
    EXPECT_EQ(net.edges(vertex_to_remove)->size(), 0ul);
    EXPECT_EQ(net.edges(1, vertex_to_remove)->size(), 0ul);
    EXPECT_EQ(net.edges(vertex_to_remove, 1)->size(), 0ul);

    // Bring back the whole graph
    EXPECT_TRUE(net.upsert(edge1));
    EXPECT_TRUE(net.upsert(edge2));
    EXPECT_TRUE(net.upsert(edge3));
    EXPECT_TRUE(*net.contains(vertex_to_remove));
    EXPECT_EQ(net.edges(vertex_to_remove)->size(), 2ul);
    EXPECT_EQ(net.edges(1, vertex_to_remove)->size(), 1ul);
    EXPECT_EQ(net.edges(vertex_to_remove, 1)->size(), 0ul);
    EXPECT_TRUE(db.clear());
}

TEST(db, graph_triangle_batch_api) {

    database_t db;
    EXPECT_TRUE(db.open(""));

    collection_t main = *db.collection();
    graph_ref_t net = main.as_graph();

    std::vector<edge_t> triangle {
        {1, 2, 9},
        {2, 3, 10},
        {3, 1, 11},
    };

    EXPECT_TRUE(net.upsert(edges(triangle)));
    EXPECT_TRUE(*net.contains(1));
    EXPECT_TRUE(*net.contains(2));
    EXPECT_FALSE(*net.contains(9));
    EXPECT_FALSE(*net.contains(10));
    EXPECT_FALSE(*net.contains(1000));

    EXPECT_EQ(*net.degree(1), 2u);
    EXPECT_EQ(*net.degree(2), 2u);
    EXPECT_EQ(*net.degree(3), 2u);
    EXPECT_EQ(*net.degree(1, ukv_vertex_source_k), 1u);
    EXPECT_EQ(*net.degree(2, ukv_vertex_source_k), 1u);
    EXPECT_EQ(*net.degree(3, ukv_vertex_source_k), 1u);

    EXPECT_TRUE(net.edges(1));
    EXPECT_EQ(net.edges(1)->size(), 2ul);
    EXPECT_EQ(net.edges(1, ukv_vertex_source_k)->size(), 1ul);
    EXPECT_EQ(net.edges(1, ukv_vertex_target_k)->size(), 1ul);

    EXPECT_EQ(net.edges(3, ukv_vertex_target_k)->size(), 1ul);
    EXPECT_EQ(net.edges(2, ukv_vertex_source_k)->size(), 1ul);
    EXPECT_EQ((*net.edges(3, ukv_vertex_target_k))[0].source_id, 2);
    EXPECT_EQ((*net.edges(3, ukv_vertex_target_k))[0].target_id, 3);
    EXPECT_EQ((*net.edges(3, ukv_vertex_target_k))[0].id, 10);
    EXPECT_EQ(net.edges(3, 1)->size(), 1ul);
    EXPECT_EQ(net.edges(1, 3)->size(), 0ul);

    // Check scans
    EXPECT_TRUE(net.edges());
    {
        std::unordered_set<edge_t, edge_hash_t> expected_edges {triangle.begin(), triangle.end()};
        std::unordered_set<edge_t, edge_hash_t> exported_edges;

        auto present_edges = *net.edges();
        auto present_it = std::move(present_edges).begin();
        size_t count_results = 0ul;
        while (!present_it.is_end()) {
            exported_edges.insert(*present_it);
            ++present_it;
            ++count_results;
        }
        EXPECT_EQ(count_results, triangle.size() * 2);
        EXPECT_EQ(exported_edges, expected_edges);
    }

    // Remove a single edge, making sure that the nodes info persists
    EXPECT_TRUE(net.remove(edges_view_t {
        {&triangle[0].source_id},
        {&triangle[0].target_id},
        {&triangle[0].id},
    }));
    EXPECT_TRUE(*net.contains(1));
    EXPECT_TRUE(*net.contains(2));
    EXPECT_EQ(net.edges(1, 2)->size(), 0ul);

    // Bring that edge back
    EXPECT_TRUE(net.upsert(edges_view_t {
        {&triangle[0].source_id},
        {&triangle[0].target_id},
        {&triangle[0].id},
    }));
    EXPECT_EQ(net.edges(1, 2)->size(), 1ul);

    // Remove a vertex
    ukv_key_t vertex_to_remove = 2;
    EXPECT_TRUE(net.remove(vertex_to_remove));
    EXPECT_FALSE(*net.contains(vertex_to_remove));
    EXPECT_EQ(net.edges(vertex_to_remove)->size(), 0ul);
    EXPECT_EQ(net.edges(1, vertex_to_remove)->size(), 0ul);
    EXPECT_EQ(net.edges(vertex_to_remove, 1)->size(), 0ul);

    // Bring back the whole graph
    EXPECT_TRUE(net.upsert(edges(triangle)));
    EXPECT_TRUE(*net.contains(vertex_to_remove));
    EXPECT_EQ(net.edges(vertex_to_remove)->size(), 2ul);
    EXPECT_EQ(net.edges(1, vertex_to_remove)->size(), 1ul);
    EXPECT_EQ(net.edges(vertex_to_remove, 1)->size(), 0ul);
    EXPECT_TRUE(db.clear());
}

edge_t make_edge(ukv_key_t edge_id, ukv_key_t v1, ukv_key_t v2) {
    return {v1, v2, edge_id};
}

std::vector<edge_t> make_edges(std::size_t vertices_count = 2, std::size_t next_connect = 1) {
    std::vector<edge_t> es;
    ukv_key_t edge_id = 0;
    for (ukv_key_t vertex_id = 0; vertex_id != vertices_count; ++vertex_id) {
        ukv_key_t connect_with = vertex_id + next_connect;
        while (connect_with < vertices_count) {
            edge_id++;
            es.push_back(make_edge(edge_id, vertex_id, connect_with));
            connect_with = connect_with + next_connect;
        }
    }
    return es;
}

TEST(db, graph_random_fill) {
    database_t db;
    EXPECT_TRUE(db.open(""));

    collection_t main = *db.collection();
    graph_ref_t graph = main.as_graph();

    constexpr std::size_t vertices_count = 1000;
    auto edges_vec = make_edges(vertices_count, 100);
    EXPECT_TRUE(graph.upsert(edges(edges_vec)));

    for (ukv_key_t vertex_id = 0; vertex_id != vertices_count; ++vertex_id) {
        EXPECT_TRUE(graph.contains(vertex_id));
        EXPECT_EQ(*graph.degree(vertex_id), 9u);
    }
}
#if 0
TEST(db, graph_remove_vertices) {
    database_t db;
    EXPECT_TRUE(db.open(""));

    collection_t main = *db.collection();
    graph_ref_t graph = main.as_graph();

    constexpr std::size_t vertices_count = 2;
    auto edges_vec = make_edges(vertices_count, 1);
    EXPECT_TRUE(graph.upsert(edges(edges_vec)));

    for (ukv_key_t vertex_id = 0; vertex_id != vertices_count; ++vertex_id) {
        EXPECT_TRUE(graph.contains(vertex_id));
        EXPECT_TRUE(*graph.contains(vertex_id));
        EXPECT_TRUE(graph.remove(vertex_id));
        EXPECT_TRUE(graph.contains(vertex_id));
        EXPECT_FALSE(*graph.contains(vertex_id));
    }
}

TEST(db, graph_remove_edges_keep_vertices) {
    database_t db;
    EXPECT_TRUE(db.open(""));

    collection_t main = *db.collection();
    graph_ref_t graph = main.as_graph();

    constexpr std::size_t vertices_count = 1000;
    auto edges_vec = make_edges(vertices_count, 100);
    EXPECT_TRUE(graph.upsert(edges(edges_vec)));
    EXPECT_TRUE(graph.remove(edges(edges_vec)));

    for (ukv_key_t vertex_id = 0; vertex_id != vertices_count; ++vertex_id) {
        EXPECT_TRUE(graph.contains(vertex_id));
        EXPECT_TRUE(*graph.contains(vertex_id));
    }
}
#endif
int main(int argc, char** argv) {
    std::filesystem::create_directory("./tmp");
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}