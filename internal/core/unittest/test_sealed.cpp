// Copyright (C) 2019-2020 Zilliz. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except in compliance
// with the License. You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied. See the License for the specific language governing permissions and limitations under the License

//
// Created by mike on 12/28/20.
//
#include "test_utils/DataGen.h"
#include <gtest/gtest.h>
#include <knowhere/index/vector_index/VecIndex.h>
#include <knowhere/index/vector_index/adapter/VectorAdapter.h>
#include <knowhere/index/vector_index/VecIndexFactory.h>
#include <knowhere/index/vector_index/IndexIVF.h>
#include "segcore/SegmentSealedImpl.h"

using namespace milvus;
using namespace milvus::segcore;
using namespace milvus::query;

TEST(Sealed, without_predicate) {
    using namespace milvus::query;
    using namespace milvus::segcore;
    auto schema = std::make_shared<Schema>();
    auto dim = 16;
    auto topK = 5;
    auto metric_type = MetricType::METRIC_L2;
    auto fake_id = schema->AddDebugField("fakevec", DataType::VECTOR_FLOAT, dim, metric_type);
    schema->AddDebugField("age", DataType::FLOAT);
    std::string dsl = R"({
        "bool": {
            "must": [
            {
                "vector": {
                    "fakevec": {
                        "metric_type": "L2",
                        "params": {
                            "nprobe": 10
                        },
                        "query": "$0",
                        "topk": 5
                    }
                }
            }
            ]
        }
    })";

    int64_t N = 1000 * 1000;

    auto dataset = DataGen(schema, N);
    auto vec_col = dataset.get_col<float>(0);
    for (int64_t i = 0; i < 1000 * dim; ++i) {
        vec_col.push_back(0);
    }
    auto query_ptr = vec_col.data() + 4200 * dim;
    auto segment = CreateGrowingSegment(schema);
    segment->PreInsert(N);
    segment->Insert(0, N, dataset.row_ids_.data(), dataset.timestamps_.data(), dataset.raw_);

    auto plan = CreatePlan(*schema, dsl);
    auto num_queries = 5;
    auto ph_group_raw = CreatePlaceholderGroupFromBlob(num_queries, 16, query_ptr);
    auto ph_group = ParsePlaceholderGroup(plan.get(), ph_group_raw.SerializeAsString());

    QueryResult qr;
    Timestamp time = 1000000;
    std::vector<const PlaceholderGroup*> ph_group_arr = {ph_group.get()};

    qr = segment->Search(plan.get(), ph_group_arr.data(), &time, 1);
    auto pre_result = QueryResultToJson(qr);
    auto indexing = std::make_shared<knowhere::IVF>();

    auto conf = knowhere::Config{{knowhere::meta::DIM, dim},
                                 {knowhere::meta::TOPK, topK},
                                 {knowhere::IndexParams::nlist, 100},
                                 {knowhere::IndexParams::nprobe, 10},
                                 {knowhere::Metric::TYPE, milvus::knowhere::Metric::L2},
                                 {knowhere::meta::DEVICEID, 0}};

    auto database = knowhere::GenDataset(N, dim, vec_col.data() + 1000 * dim);
    indexing->Train(database, conf);
    indexing->AddWithoutIds(database, conf);

    EXPECT_EQ(indexing->Count(), N);
    EXPECT_EQ(indexing->Dim(), dim);

    auto query_dataset = knowhere::GenDataset(num_queries, dim, query_ptr);

    auto result = indexing->Query(query_dataset, conf, nullptr);

    auto ids = result->Get<int64_t*>(milvus::knowhere::meta::IDS);     // for comparison
    auto dis = result->Get<float*>(milvus::knowhere::meta::DISTANCE);  // for comparison
    std::vector<int64_t> vec_ids(ids, ids + topK * num_queries);
    std::vector<float> vec_dis(dis, dis + topK * num_queries);

    qr.internal_seg_offsets_ = vec_ids;
    qr.result_distances_ = vec_dis;
    auto ref_result = QueryResultToJson(qr);

    LoadIndexInfo load_info;
    load_info.field_name = "fakevec";
    load_info.field_id = fake_id.get();
    load_info.index = indexing;
    load_info.index_params["metric_type"] = "L2";

    segment->LoadIndexing(load_info);
    qr = QueryResult();

    qr = segment->Search(plan.get(), ph_group_arr.data(), &time, 1);

    auto post_result = QueryResultToJson(qr);
    std::cout << ref_result.dump(1);
    std::cout << post_result.dump(1);
    ASSERT_EQ(ref_result.dump(2), post_result.dump(2));
}

TEST(Sealed, with_predicate) {
    using namespace milvus::query;
    using namespace milvus::segcore;
    auto schema = std::make_shared<Schema>();
    auto dim = 16;
    auto topK = 5;
    auto metric_type = MetricType::METRIC_L2;
    auto fake_id = schema->AddDebugField("fakevec", DataType::VECTOR_FLOAT, dim, metric_type);
    schema->AddDebugField("counter", DataType::INT64);
    std::string dsl = R"({
        "bool": {
            "must": [
            {
                "range": {
                    "counter": {
                        "GE": 420000,
                        "LT": 420005
                    }
                }
            },
            {
                "vector": {
                    "fakevec": {
                        "metric_type": "L2",
                        "params": {
                            "nprobe": 10
                        },
                        "query": "$0",
                        "topk": 5
                    }
                }
            }
            ]
        }
    })";

    int64_t N = 1000 * 1000;

    auto dataset = DataGen(schema, N);
    auto vec_col = dataset.get_col<float>(0);
    auto query_ptr = vec_col.data() + 420000 * dim;
    auto segment = CreateGrowingSegment(schema);
    segment->PreInsert(N);
    segment->Insert(0, N, dataset.row_ids_.data(), dataset.timestamps_.data(), dataset.raw_);

    auto plan = CreatePlan(*schema, dsl);
    auto num_queries = 5;
    auto ph_group_raw = CreatePlaceholderGroupFromBlob(num_queries, 16, query_ptr);
    auto ph_group = ParsePlaceholderGroup(plan.get(), ph_group_raw.SerializeAsString());

    QueryResult qr;
    Timestamp time = 10000000;
    std::vector<const PlaceholderGroup*> ph_group_arr = {ph_group.get()};

    qr = segment->Search(plan.get(), ph_group_arr.data(), &time, 1);
    auto pre_qr = qr;
    auto indexing = std::make_shared<knowhere::IVF>();

    auto conf = knowhere::Config{{knowhere::meta::DIM, dim},
                                 {knowhere::meta::TOPK, topK},
                                 {knowhere::IndexParams::nlist, 100},
                                 {knowhere::IndexParams::nprobe, 10},
                                 {knowhere::Metric::TYPE, milvus::knowhere::Metric::L2},
                                 {knowhere::meta::DEVICEID, 0}};

    auto database = knowhere::GenDataset(N, dim, vec_col.data());
    indexing->Train(database, conf);
    indexing->AddWithoutIds(database, conf);

    EXPECT_EQ(indexing->Count(), N);
    EXPECT_EQ(indexing->Dim(), dim);

    auto query_dataset = knowhere::GenDataset(num_queries, dim, query_ptr);

    auto result = indexing->Query(query_dataset, conf, nullptr);

    LoadIndexInfo load_info;
    load_info.field_name = "fakevec";
    load_info.field_id = fake_id.get();
    load_info.index = indexing;
    load_info.index_params["metric_type"] = "L2";

    segment->LoadIndexing(load_info);
    qr = QueryResult();

    qr = segment->Search(plan.get(), ph_group_arr.data(), &time, 1);

    auto post_qr = qr;
    for (int i = 0; i < num_queries; ++i) {
        auto offset = i * topK;
        ASSERT_EQ(post_qr.internal_seg_offsets_[offset], 420000 + i);
        ASSERT_EQ(post_qr.result_distances_[offset], 0.0);
    }
}

void
SealedLoader(const GeneratedData& dataset, SegmentSealed& seg) {
    // TODO
    auto row_count = dataset.row_ids_.size();
    {
        LoadFieldDataInfo info;
        info.blob = dataset.row_ids_.data();
        info.row_count = dataset.row_ids_.size();
        info.field_id = 0;  // field id for RowId
        seg.LoadFieldData(info);
    }
    int field_offset = 0;
    for (auto& meta : seg.get_schema().get_fields()) {
        if (meta.is_vector()) {
            ++field_offset;
            continue;
        }
        LoadFieldDataInfo info;
        info.field_id = meta.get_id().get();
        info.row_count = row_count;
        info.blob = dataset.cols_[field_offset].data();
        seg.LoadFieldData(info);
        ++field_offset;
    }
}

TEST(Sealed, LoadFieldData) {
    auto dim = 16;
    auto topK = 5;
    int64_t N = 1000 * 1000;
    auto metric_type = MetricType::METRIC_L2;
    auto schema = std::make_shared<Schema>();
    auto fakevec_id = schema->AddDebugField("fakevec", DataType::VECTOR_FLOAT, dim, metric_type);
    schema->AddDebugField("counter", DataType::INT64);
    schema->AddDebugField("double", DataType::DOUBLE);

    auto dataset = DataGen(schema, N);

    auto fakevec = dataset.get_col<float>(0);

    auto conf = knowhere::Config{{knowhere::meta::DIM, dim},
                                 {knowhere::meta::TOPK, topK},
                                 {knowhere::IndexParams::nlist, 100},
                                 {knowhere::IndexParams::nprobe, 10},
                                 {knowhere::Metric::TYPE, milvus::knowhere::Metric::L2},
                                 {knowhere::meta::DEVICEID, 0}};
    auto database = knowhere::GenDataset(N, dim, fakevec.data());
    auto indexing = std::make_shared<knowhere::IVF>();
    indexing->Train(database, conf);
    indexing->AddWithoutIds(database, conf);

    auto segment = CreateSealedSegment(schema);
    SealedLoader(dataset, *segment);
    {
        LoadIndexInfo vec_info;
        vec_info.field_id = fakevec_id.get();
        vec_info.field_name = "fakevec";
        vec_info.index = indexing;
        vec_info.index_params["metric_type"] = milvus::knowhere::Metric::L2;
        segment->LoadIndex(vec_info);
    }
    ASSERT_EQ(segment->num_chunk_data(), 1);
    auto chunk_span1 = segment->chunk_data<int64_t>(FieldOffset(1), 0);
    auto chunk_span2 = segment->chunk_data<double>(FieldOffset(2), 0);
    auto ref1 = dataset.get_col<int64_t>(1);
    auto ref2 = dataset.get_col<double>(2);
    for (int i = 0; i < N; ++i) {
        ASSERT_EQ(chunk_span1[i], ref1[i]);
        ASSERT_EQ(chunk_span2[i], ref2[i]);
    }
    std::string dsl = R"({
        "bool": {
            "must": [
            {
                "range": {
                    "double": {
                        "GE": -1,
                        "LT": 1
                    }
                }
            },
            {
                "vector": {
                    "fakevec": {
                        "metric_type": "L2",
                        "params": {
                            "nprobe": 10
                        },
                        "query": "$0",
                        "topk": 5
                    }
                }
            }
            ]
        }
    })";

    auto plan = CreatePlan(*schema, dsl);
    auto num_queries = 5;
    auto ph_group_raw = CreatePlaceholderGroup(num_queries, 16, 1024);
    auto ph_group = ParsePlaceholderGroup(plan.get(), ph_group_raw.SerializeAsString());
    Timestamp time = 1000000;
    std::vector<const PlaceholderGroup*> ph_group_arr = {ph_group.get()};

    auto qr = segment->Search(plan.get(), ph_group_arr.data(), &time, 1);
    auto json = QueryResultToJson(qr);
    std::cout << json.dump(1);
}
