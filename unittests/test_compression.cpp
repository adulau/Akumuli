#include <iostream>

#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE Main
#include <boost/test/unit_test.hpp>
#include <vector>

#include "storage_engine/compression.h"
#include "storage_engine/volume.h"


using namespace Akumuli;

static const u64
    EXPECTED[] = {
    0ul, 1ul, 10ul,
    67ul, 127ul, 128ul,
    1024ul, 10000ul,
    100000ul, 420000000ul,
    420000001ul
};

static const size_t
        EXPECTED_SIZE = sizeof(EXPECTED)/sizeof(u64);

template<class TStreamWriter>
void test_stream_write(TStreamWriter& writer) {
    // Encode
    for (auto i = 0u; i < EXPECTED_SIZE; i++) {
        if (!writer.put(EXPECTED[i])) {
            BOOST_FAIL("Buffer is too small");
        }
    }
    writer.commit();

    const size_t
            USED_SIZE = writer.size();
    BOOST_REQUIRE_LT(USED_SIZE, sizeof(EXPECTED));
    BOOST_REQUIRE_GT(USED_SIZE, EXPECTED_SIZE);
}

template<class TVal, class TStreamWriter, class TStreamReader>
void test_stream_chunked_op(TStreamWriter& writer, TStreamReader& reader, size_t nsteps, bool sort_input=false, bool fixed_step=false) {
    std::vector<TVal> input;
    const size_t step_size = 16;
    const size_t input_size = step_size*nsteps;
    TVal value = 100000;

    // Generate
    if (!fixed_step) {
        input.push_back(0);
        for (u32 i = 0; i < (input_size-1); i++) {
            int delta = rand() % 1000 - 500;
            value += TVal(delta);
            input.push_back(value);
        }
    } else {
        for (u32 i = 0; i < nsteps; i++) {
            int delta = rand() % 1000;  // all positive
            for (u32 j = 0; j < step_size; j++) {
                value += TVal(delta);
                input.push_back(value);
            }
        }
    }

    if (sort_input) {
        std::sort(input.begin(), input.end());
    }

    // Encode
    for (auto offset = 0u; offset < input_size; offset += step_size) {
        auto success = writer.tput(input.data() + offset, step_size);
        if (!success) {
            BOOST_REQUIRE(success);
        }
    }

    // Decode and compare results
    std::vector<TVal> results;
    for (auto offset = 0ul; offset < input_size; offset++) {
        auto next = reader.next();
        results.push_back(next);
    }

    BOOST_REQUIRE_EQUAL_COLLECTIONS(input.begin(), input.end(),
                                    results.begin(), results.end());
}

template<class TStreamReader>
void test_stream_read(TStreamReader& reader) {
    // Read it back
    u64 actual[EXPECTED_SIZE];
    for (auto i = 0u; i < EXPECTED_SIZE; i++) {
        actual[i] = reader.next();
    }
    BOOST_REQUIRE_EQUAL_COLLECTIONS(EXPECTED, EXPECTED + EXPECTED_SIZE,
            actual, actual + EXPECTED_SIZE);
}

//! Base128StreamReader::next is a template, so we need to specialize this function.
template<>
void test_stream_read(Base128StreamReader& reader) {
    // Read it back
    u64 actual[EXPECTED_SIZE];
    for (auto i = 0u; i < EXPECTED_SIZE; i++) {
        actual[i] = reader.template next<u64>();
    }
    BOOST_REQUIRE_EQUAL_COLLECTIONS(EXPECTED, EXPECTED + EXPECTED_SIZE,
            actual, actual + EXPECTED_SIZE);
}

//! VByteStreamReader::next is a template, so we need to specialize this function.
template<>
void test_stream_read(VByteStreamReader& reader) {
    // Read it back
    u64 actual[EXPECTED_SIZE];
    for (auto i = 0u; i < EXPECTED_SIZE; i++) {
        actual[i] = reader.template next<u64>();
    }
    BOOST_REQUIRE_EQUAL_COLLECTIONS(EXPECTED, EXPECTED + EXPECTED_SIZE,
            actual, actual + EXPECTED_SIZE);
}

BOOST_AUTO_TEST_CASE(Test_base128) {

    std::vector<unsigned char> data;
    data.resize(1000);

    Base128StreamWriter writer(data.data(), data.data() + data.size());
    test_stream_write(writer);

    Base128StreamReader reader(data.data(), data.data() + data.size());
    test_stream_read(reader);
}

BOOST_AUTO_TEST_CASE(Test_vbyte) {

    std::vector<unsigned char> data;
    data.resize(1000);

    VByteStreamWriter writer(data.data(), data.data() + data.size());
    test_stream_write(writer);

    VByteStreamReader reader(data.data(), data.data() + data.size());
    test_stream_read(reader);
}

BOOST_AUTO_TEST_CASE(Test_chunked_delta_delta_vbyte_0) {
    std::vector<unsigned char> data;
    data.resize(4*1024);  // 4KB of storage

    {   // variable step
        VByteStreamWriter wstream(data.data(), data.data() + data.size());
        DeltaDeltaStreamWriter<16, u64> delta_writer(wstream);
        VByteStreamReader rstream(data.data(), data.data() + data.size());
        DeltaDeltaStreamReader<16, u64> delta_reader(rstream);

        test_stream_chunked_op<u64>(delta_writer, delta_reader, 100, true, false);
    }
    {   // fixed step
        VByteStreamWriter wstream(data.data(), data.data() + data.size());
        DeltaDeltaStreamWriter<16, u64> delta_writer(wstream);
        VByteStreamReader rstream(data.data(), data.data() + data.size());
        DeltaDeltaStreamReader<16, u64> delta_reader(rstream);

        test_stream_chunked_op<u64>(delta_writer, delta_reader, 100, true, true);
    }
}

//! Generate time-series from random walk
struct RandomWalk {
    std::random_device                  randdev;
    std::mt19937                        generator;
    std::normal_distribution<double>    distribution;
    double                              value;

    RandomWalk(double start, double mean, double stddev)
        : generator(randdev())
        , distribution(mean, stddev)
        , value(start)
    {
    }

    double generate() {
        value += distribution(generator);
        return value;
    }
};


void test_float_compression(double start, std::vector<double>* psrc=nullptr) {
    RandomWalk rwalk(start, 1., .11);
    int N = 10000;
    std::vector<double> samples;
    std::vector<u8> block;
    block.resize(N*9, 0);

    // Compress
    VByteStreamWriter wstream(block.data(), block.data() + block.size());
    FcmStreamWriter<> writer(wstream);
    if (psrc == nullptr) {
        double val = rwalk.generate();
        samples.push_back(val);
    } else {
        samples = *psrc;
    }

    for (size_t ix = 0; ix < samples.size(); ix++) {
        auto val = samples.at(ix);
        writer.put(val);
    }
    writer.commit();

    // Decompress
    VByteStreamReader rstream(block.data(), block.data() + block.size());
    FcmStreamReader reader(rstream);
    for (size_t ix = 0; ix < samples.size(); ix++) {
        double val = reader.next();
        if (val != samples.at(ix)) {
            BOOST_REQUIRE(val == samples.at(ix));
        }
    }
}

BOOST_AUTO_TEST_CASE(Test_float_compression_0) {
    test_float_compression(0);
}

BOOST_AUTO_TEST_CASE(Test_float_compression_1) {
    test_float_compression(1E-100);
}

BOOST_AUTO_TEST_CASE(Test_float_compression_2) {
    test_float_compression(1E100);
}

BOOST_AUTO_TEST_CASE(Test_float_compression_3) {
    test_float_compression(-1E-100);
}

BOOST_AUTO_TEST_CASE(Test_float_compression_4) {
    test_float_compression(-1E100);
}

BOOST_AUTO_TEST_CASE(Test_float_compression_5) {
    std::vector<double> samples(998, 3.14159);
    samples.push_back(111.222);
    samples.push_back(222.333);
    test_float_compression(0, &samples);
}

void test_block_compression(double start, unsigned N=10000, bool regullar=false) {
    RandomWalk rwalk(start, 1., .11);
    std::vector<aku_Timestamp> timestamps;
    std::vector<double> values;
    std::vector<u8> block;
    block.resize(4096);

    if (regullar) {
        aku_Timestamp its = static_cast<aku_Timestamp>(rand());
        aku_Timestamp stp = static_cast<aku_Timestamp>(rand() % 1000);
        for (unsigned i = 0; i < N; i++) {
            values.push_back(rwalk.generate());
            its += stp;
            timestamps.push_back(its);
        }
    } else {
        aku_Timestamp its = static_cast<aku_Timestamp>(rand());
        for (unsigned i = 0; i < N; i++) {
            values.push_back(rwalk.generate());
            u32 skew = rand() % 100;
            its += skew;
            timestamps.push_back(its);
        }
    }

    // compress

    StorageEngine::DataBlockWriter writer(42, block.data(), block.size());

    size_t actual_nelements = 0ull;
    bool writer_overflow = false;
    for (size_t ix = 0; ix < N; ix++) {
        aku_Status status = writer.put(timestamps.at(ix), values.at(ix));
        if (status == AKU_EOVERFLOW) {
            // Block is full
            actual_nelements = ix;
            writer_overflow = true;
            break;
        }
        BOOST_REQUIRE_EQUAL(status, AKU_SUCCESS);
    }
    if (!writer_overflow) {
        actual_nelements = N;
    }
    size_t size_used = writer.commit();

    // decompress
    StorageEngine::DataBlockReader reader(block.data(), size_used);

    std::vector<aku_Timestamp> out_timestamps;
    std::vector<double> out_values;

    // gen number of elements stored in block
    u32 nelem = reader.nelements();
    BOOST_REQUIRE_EQUAL(nelem, actual_nelements);
    BOOST_REQUIRE_NE(nelem, 0);

    BOOST_REQUIRE_EQUAL(reader.get_id(), 42);
    for (size_t ix = 0ull; ix < reader.nelements(); ix++) {
        aku_Status status;
        aku_Timestamp  ts;
        double      value;
        std::tie(status, ts, value) = reader.next();
        BOOST_REQUIRE_EQUAL(status, AKU_SUCCESS);
        out_timestamps.push_back(ts);
        out_values.push_back(value);
    }

    // nelements() + 1 call should result in error
    aku_Status status;
    aku_Timestamp  ts;
    double      value;
    std::tie(status, ts, value) = reader.next();
    BOOST_REQUIRE_EQUAL(status, AKU_ENO_DATA);

    for (size_t i = 0; i < nelem; i++) {
        if (timestamps.at(i) != out_timestamps.at(i)) {
            BOOST_FAIL("Bad timestamp at " << i << ", expected: " << timestamps.at(i) <<
                       ", actual: " << out_timestamps.at(i));
        }
        if (values.at(i) != out_values.at(i)) {
            BOOST_FAIL("Bad value at " << i << ", expected: " << values.at(i) <<
                       ", actual: " << out_values.at(i));
        }
    }
}

BOOST_AUTO_TEST_CASE(Test_block_compression_00) {
    test_block_compression(0);
}

BOOST_AUTO_TEST_CASE(Test_block_compression_01) {
    test_block_compression(1E-100);
}

BOOST_AUTO_TEST_CASE(Test_block_compression_02) {
    test_block_compression(1E100);
}

BOOST_AUTO_TEST_CASE(Test_block_compression_03) {
    test_block_compression(-1E-100);
}

BOOST_AUTO_TEST_CASE(Test_block_compression_04) {
    test_block_compression(-1E100);
}

BOOST_AUTO_TEST_CASE(Test_block_compression_05) {
    test_block_compression(0, 1);
}

BOOST_AUTO_TEST_CASE(Test_block_compression_06) {
    test_block_compression(0, 16);
}

BOOST_AUTO_TEST_CASE(Test_block_compression_07) {
    test_block_compression(0, 100);
}

BOOST_AUTO_TEST_CASE(Test_block_compression_08) {
    test_block_compression(0, 0x100);
}

BOOST_AUTO_TEST_CASE(Test_block_compression_09) {
    test_block_compression(0, 0x111);
}

BOOST_AUTO_TEST_CASE(Test_block_compression_10) {
    test_block_compression(0, 10000, true);
}

BOOST_AUTO_TEST_CASE(Test_block_compression_11) {
    test_block_compression(1E-100, 10000, true);
}

BOOST_AUTO_TEST_CASE(Test_block_compression_12) {
    test_block_compression(1E100, 10000, true);
}

BOOST_AUTO_TEST_CASE(Test_block_compression_13) {
    test_block_compression(-1E-100, 10000, true);
}

BOOST_AUTO_TEST_CASE(Test_block_compression_14) {
    test_block_compression(-1E100, 10000, true);
}

BOOST_AUTO_TEST_CASE(Test_block_compression_15) {
    test_block_compression(0, 1, true);
}

BOOST_AUTO_TEST_CASE(Test_block_compression_16) {
    test_block_compression(0, 16, true);
}

BOOST_AUTO_TEST_CASE(Test_block_compression_17) {
    test_block_compression(0, 100, true);
}

BOOST_AUTO_TEST_CASE(Test_block_compression_18) {
    test_block_compression(0, 0x100, true);
}

BOOST_AUTO_TEST_CASE(Test_block_compression_19) {
    test_block_compression(0, 0x111, true);
}

void test_block_iovec_compression(double start, unsigned N=10000, bool regullar=false) {
    RandomWalk rwalk(start, 1., .11);
    std::vector<aku_Timestamp> timestamps;
    std::vector<double> values;
    std::unique_ptr<StorageEngine::IOVecBlock> block;
    block.reset(new StorageEngine::IOVecBlock());

    if (regullar) {
        aku_Timestamp its = static_cast<aku_Timestamp>(rand());
        aku_Timestamp stp = static_cast<aku_Timestamp>(rand() % 1000);
        for (unsigned i = 0; i < N; i++) {
            values.push_back(rwalk.generate());
            its += stp;
            timestamps.push_back(its);
        }
    } else {
        aku_Timestamp its = static_cast<aku_Timestamp>(rand());
        for (unsigned i = 0; i < N; i++) {
            values.push_back(rwalk.generate());
            u32 skew = rand() % 100;
            its += skew;
            timestamps.push_back(its);
        }
    }

    // compress

    StorageEngine::IOVecBlockWriter<StorageEngine::IOVecBlock> writer(block.get());
    writer.init(42);

    size_t actual_nelements = 0ull;
    bool writer_overflow = false;
    for (size_t ix = 0; ix < N; ix++) {
        aku_Status status = writer.put(timestamps.at(ix), values.at(ix));
        if (status == AKU_EOVERFLOW) {
            // Block is full
            actual_nelements = ix;
            writer_overflow = true;
            break;
        }
        BOOST_REQUIRE_EQUAL(status, AKU_SUCCESS);
    }
    if (!writer_overflow) {
        actual_nelements = N;
    }
    size_t size_used = writer.commit();
    AKU_UNUSED(size_used);

    // decompress using normal procedure
    std::vector<u8> cblock;
    cblock.reserve(4096);
    for (int i = 0; i < StorageEngine::IOVecBlock::NCOMPONENTS; i++) {
        const int sz = StorageEngine::IOVecBlock::COMPONENT_SIZE;
        std::copy(block->get_cdata(i), block->get_cdata(i) + sz, std::back_inserter(cblock));
    }
    StorageEngine::DataBlockReader reader(cblock.data(), cblock.size());

    std::vector<aku_Timestamp> out_timestamps;
    std::vector<double> out_values;

    // gen number of elements stored in block
    auto nelem = reader.nelements();
    BOOST_REQUIRE_EQUAL(nelem, actual_nelements);
    BOOST_REQUIRE_NE(nelem, 0);

    BOOST_REQUIRE_EQUAL(reader.get_id(), 42);
    for (size_t ix = 0ull; ix < reader.nelements(); ix++) {
        aku_Status status;
        aku_Timestamp  ts;
        double      value;
        std::tie(status, ts, value) = reader.next();
        BOOST_REQUIRE_EQUAL(status, AKU_SUCCESS);
        out_timestamps.push_back(ts);
        out_values.push_back(value);
    }

    // nelements() + 1 call should result in error
    aku_Status status;
    aku_Timestamp  ts;
    double      value;
    std::tie(status, ts, value) = reader.next();
    BOOST_REQUIRE_EQUAL(status, AKU_ENO_DATA);

    for (size_t i = 0; i < nelem; i++) {
        if (timestamps.at(i) != out_timestamps.at(i)) {
            BOOST_FAIL("Bad timestamp at " << i << ", expected: " << timestamps.at(i) <<
                       ", actual: " << out_timestamps.at(i));
        }
        if (values.at(i) != out_values.at(i)) {
            BOOST_FAIL("Bad value at " << i << ", expected: " << values.at(i) <<
                       ", actual: " << out_values.at(i));
        }
    }
}

BOOST_AUTO_TEST_CASE(Test_iovec_compression_00) {
    test_block_iovec_compression(0);
}

BOOST_AUTO_TEST_CASE(Test_iovec_compression_01) {
    test_block_iovec_compression(1E-100);
}
