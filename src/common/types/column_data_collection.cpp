#include "duckdb/common/types/column_data_collection.hpp"

#include "duckdb/common/printer.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/column_data_collection_segment.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/storage/buffer_manager.hpp"

namespace duckdb {

struct ColumnDataMetaData;

typedef void (*column_data_copy_function_t)(ColumnDataMetaData &meta_data, const UnifiedVectorFormat &source_data,
                                            Vector &source, idx_t offset, idx_t copy_count);

struct ColumnDataCopyFunction {
	column_data_copy_function_t function;
	vector<ColumnDataCopyFunction> child_functions;
};

struct ColumnDataMetaData {
	ColumnDataMetaData(ColumnDataCopyFunction &copy_function, ColumnDataCollectionSegment &segment,
	                   ColumnDataAppendState &state, ChunkMetaData &chunk_data, VectorDataIndex vector_data_index)
	    : copy_function(copy_function), segment(segment), state(state), chunk_data(chunk_data),
	      vector_data_index(vector_data_index) {
	}
	ColumnDataMetaData(ColumnDataCopyFunction &copy_function, ColumnDataMetaData &parent,
	                   VectorDataIndex vector_data_index)
	    : copy_function(copy_function), segment(parent.segment), state(parent.state), chunk_data(parent.chunk_data),
	      vector_data_index(vector_data_index) {
	}

	ColumnDataCopyFunction &copy_function;
	ColumnDataCollectionSegment &segment;
	ColumnDataAppendState &state;
	ChunkMetaData &chunk_data;
	VectorDataIndex vector_data_index;
	idx_t child_list_size = DConstants::INVALID_INDEX;

	VectorMetaData &GetVectorMetaData() {
		return segment.GetVectorData(vector_data_index);
	}
};

//! Explicitly initialized without types
ColumnDataCollection::ColumnDataCollection(Allocator &allocator_p) {
	types.clear();
	count = 0;
	this->finished_append = false;
	allocator = make_shared<ColumnDataAllocator>(allocator_p);
}

ColumnDataCollection::ColumnDataCollection(Allocator &allocator_p, vector<LogicalType> types_p) {
	Initialize(move(types_p));
	allocator = make_shared<ColumnDataAllocator>(allocator_p);
}

ColumnDataCollection::ColumnDataCollection(BufferManager &buffer_manager, vector<LogicalType> types_p) {
	Initialize(move(types_p));
	allocator = make_shared<ColumnDataAllocator>(buffer_manager);
}

ColumnDataCollection::ColumnDataCollection(shared_ptr<ColumnDataAllocator> allocator_p, vector<LogicalType> types_p) {
	Initialize(move(types_p));
	this->allocator = move(allocator_p);
}

ColumnDataCollection::ColumnDataCollection(ClientContext &context, vector<LogicalType> types_p,
                                           ColumnDataAllocatorType type)
    : ColumnDataCollection(make_shared<ColumnDataAllocator>(context, type), move(types_p)) {
	D_ASSERT(!types.empty());
}

ColumnDataCollection::ColumnDataCollection(ColumnDataCollection &other)
    : ColumnDataCollection(other.allocator, other.types) {
	other.finished_append = true;
	D_ASSERT(!types.empty());
}

ColumnDataCollection::~ColumnDataCollection() {
}

void ColumnDataCollection::Initialize(vector<LogicalType> types_p) {
	this->types = move(types_p);
	this->count = 0;
	this->finished_append = false;
	D_ASSERT(!types.empty());
	copy_functions.reserve(types.size());
	for (auto &type : types) {
		copy_functions.push_back(GetCopyFunction(type));
	}
}

void ColumnDataCollection::CreateSegment() {
	segments.emplace_back(make_unique<ColumnDataCollectionSegment>(allocator, types));
}

//===--------------------------------------------------------------------===//
// ColumnDataRow
//===--------------------------------------------------------------------===//
ColumnDataRow::ColumnDataRow(DataChunk &chunk_p, idx_t row_index, idx_t base_index)
    : chunk(chunk_p), row_index(row_index), base_index(base_index) {
}

Value ColumnDataRow::GetValue(idx_t column_index) const {
	D_ASSERT(column_index < chunk.ColumnCount());
	D_ASSERT(row_index < chunk.size());
	return chunk.data[column_index].GetValue(row_index);
}

idx_t ColumnDataRow::RowIndex() const {
	return base_index + row_index;
}

//===--------------------------------------------------------------------===//
// ColumnDataRowCollection
//===--------------------------------------------------------------------===//
ColumnDataRowCollection::ColumnDataRowCollection(const ColumnDataCollection &collection) {
	if (collection.Count() == 0) {
		return;
	}
	// read all the chunks
	ColumnDataScanState scan_state;
	collection.InitializeScan(scan_state);
	while (true) {
		auto chunk = make_unique<DataChunk>();
		collection.InitializeScanChunk(*chunk);
		if (!collection.Scan(scan_state, *chunk)) {
			break;
		}
		chunks.push_back(move(chunk));
	}
	// now create all of the column data rows
	rows.reserve(collection.Count());
	idx_t base_row = 0;
	for (auto &chunk : chunks) {
		for (idx_t row_idx = 0; row_idx < chunk->size(); row_idx++) {
			rows.emplace_back(*chunk, row_idx, base_row);
		}
		base_row += chunk->size();
	}
}

ColumnDataRow &ColumnDataRowCollection::operator[](idx_t i) {
	return rows[i];
}

const ColumnDataRow &ColumnDataRowCollection::operator[](idx_t i) const {
	return rows[i];
}

Value ColumnDataRowCollection::GetValue(idx_t column, idx_t index) const {
	return rows[index].GetValue(column);
}

//===--------------------------------------------------------------------===//
// ColumnDataChunkIterator
//===--------------------------------------------------------------------===//
ColumnDataChunkIterationHelper ColumnDataCollection::Chunks() const {
	vector<column_t> column_ids;
	for (idx_t i = 0; i < ColumnCount(); i++) {
		column_ids.push_back(i);
	}
	return Chunks(column_ids);
}

ColumnDataChunkIterationHelper ColumnDataCollection::Chunks(vector<column_t> column_ids) const {
	return ColumnDataChunkIterationHelper(*this, move(column_ids));
}

ColumnDataChunkIterationHelper::ColumnDataChunkIterationHelper(const ColumnDataCollection &collection_p,
                                                               vector<column_t> column_ids_p)
    : collection(collection_p), column_ids(move(column_ids_p)) {
}

ColumnDataChunkIterationHelper::ColumnDataChunkIterator::ColumnDataChunkIterator(
    const ColumnDataCollection *collection_p, vector<column_t> column_ids_p)
    : collection(collection_p), scan_chunk(make_shared<DataChunk>()), row_index(0) {
	if (!collection) {
		return;
	}
	collection->InitializeScan(scan_state, move(column_ids_p));
	collection->InitializeScanChunk(scan_state, *scan_chunk);
	collection->Scan(scan_state, *scan_chunk);
}

void ColumnDataChunkIterationHelper::ColumnDataChunkIterator::Next() {
	if (!collection) {
		return;
	}
	if (!collection->Scan(scan_state, *scan_chunk)) {
		collection = nullptr;
		row_index = 0;
	} else {
		row_index += scan_chunk->size();
	}
}

ColumnDataChunkIterationHelper::ColumnDataChunkIterator &
ColumnDataChunkIterationHelper::ColumnDataChunkIterator::operator++() {
	Next();
	return *this;
}

bool ColumnDataChunkIterationHelper::ColumnDataChunkIterator::operator!=(const ColumnDataChunkIterator &other) const {
	return collection != other.collection || row_index != other.row_index;
}

DataChunk &ColumnDataChunkIterationHelper::ColumnDataChunkIterator::operator*() const {
	return *scan_chunk;
}

//===--------------------------------------------------------------------===//
// ColumnDataRowIterator
//===--------------------------------------------------------------------===//
ColumnDataRowIterationHelper ColumnDataCollection::Rows() const {
	return ColumnDataRowIterationHelper(*this);
}

ColumnDataRowIterationHelper::ColumnDataRowIterationHelper(const ColumnDataCollection &collection_p)
    : collection(collection_p) {
}

ColumnDataRowIterationHelper::ColumnDataRowIterator::ColumnDataRowIterator(const ColumnDataCollection *collection_p)
    : collection(collection_p), scan_chunk(make_shared<DataChunk>()), current_row(*scan_chunk, 0, 0) {
	if (!collection) {
		return;
	}
	collection->InitializeScan(scan_state);
	collection->InitializeScanChunk(*scan_chunk);
	collection->Scan(scan_state, *scan_chunk);
}

void ColumnDataRowIterationHelper::ColumnDataRowIterator::Next() {
	if (!collection) {
		return;
	}
	current_row.row_index++;
	if (current_row.row_index >= scan_chunk->size()) {
		current_row.base_index += scan_chunk->size();
		current_row.row_index = 0;
		if (!collection->Scan(scan_state, *scan_chunk)) {
			// exhausted collection: move iterator to nop state
			current_row.base_index = 0;
			collection = nullptr;
		}
	}
}

ColumnDataRowIterationHelper::ColumnDataRowIterator ColumnDataRowIterationHelper::begin() { // NOLINT
	return ColumnDataRowIterationHelper::ColumnDataRowIterator(collection.Count() == 0 ? nullptr : &collection);
}
ColumnDataRowIterationHelper::ColumnDataRowIterator ColumnDataRowIterationHelper::end() { // NOLINT
	return ColumnDataRowIterationHelper::ColumnDataRowIterator(nullptr);
}

ColumnDataRowIterationHelper::ColumnDataRowIterator &ColumnDataRowIterationHelper::ColumnDataRowIterator::operator++() {
	Next();
	return *this;
}

bool ColumnDataRowIterationHelper::ColumnDataRowIterator::operator!=(const ColumnDataRowIterator &other) const {
	return collection != other.collection || current_row.row_index != other.current_row.row_index ||
	       current_row.base_index != other.current_row.base_index;
}

const ColumnDataRow &ColumnDataRowIterationHelper::ColumnDataRowIterator::operator*() const {
	return current_row;
}

//===--------------------------------------------------------------------===//
// Append
//===--------------------------------------------------------------------===//
void ColumnDataCollection::InitializeAppend(ColumnDataAppendState &state) {
	D_ASSERT(!finished_append);
	state.vector_data.resize(types.size());
	if (segments.empty()) {
		CreateSegment();
	}
	auto &segment = *segments.back();
	if (segment.chunk_data.empty()) {
		segment.AllocateNewChunk();
	}
	segment.InitializeChunkState(segment.chunk_data.size() - 1, state.current_chunk_state);
}

void ColumnDataCopyValidity(const UnifiedVectorFormat &source_data, validity_t *target, idx_t source_offset,
                            idx_t target_offset, idx_t copy_count) {
	ValidityMask validity(target);
	if (target_offset == 0) {
		// first time appending to this vector
		// all data here is still uninitialized
		// initialize the validity mask to set all to valid
		validity.SetAllValid(STANDARD_VECTOR_SIZE);
	}
	// FIXME: we can do something more optimized here using bitshifts & bitwise ors
	if (!source_data.validity.AllValid()) {
		for (idx_t i = 0; i < copy_count; i++) {
			auto idx = source_data.sel->get_index(source_offset + i);
			if (!source_data.validity.RowIsValid(idx)) {
				validity.SetInvalid(target_offset + i);
			}
		}
	}
}

template <class T>
struct BaseValueCopy {
	static idx_t TypeSize() {
		return sizeof(T);
	}

	template <class OP>
	static void Assign(ColumnDataMetaData &meta_data, data_ptr_t target, data_ptr_t source, idx_t target_idx,
	                   idx_t source_idx) {
		auto result_data = (T *)target;
		auto source_data = (T *)source;
		result_data[target_idx] = OP::Operation(meta_data, source_data[source_idx]);
	}
};

template <class T>
struct StandardValueCopy : public BaseValueCopy<T> {
	static T Operation(ColumnDataMetaData &, T input) {
		return input;
	}
};

struct StringValueCopy : public BaseValueCopy<string_t> {
	static string_t Operation(ColumnDataMetaData &meta_data, string_t input) {
		return input.IsInlined() ? input : meta_data.segment.heap.AddBlob(input);
	}
};

struct ListValueCopy : public BaseValueCopy<list_entry_t> {
	using TYPE = list_entry_t;

	static TYPE Operation(ColumnDataMetaData &meta_data, TYPE input) {
		input.offset += meta_data.child_list_size;
		return input;
	}
};

struct StructValueCopy {
	static idx_t TypeSize() {
		return 0;
	}

	template <class OP>
	static void Assign(ColumnDataMetaData &meta_data, data_ptr_t target, data_ptr_t source, idx_t target_idx,
	                   idx_t source_idx) {
	}
};

template <class OP>
static void TemplatedColumnDataCopy(ColumnDataMetaData &meta_data, const UnifiedVectorFormat &source_data,
                                    Vector &source, idx_t offset, idx_t count) {
	auto &segment = meta_data.segment;
	auto &append_state = meta_data.state;

	auto current_index = meta_data.vector_data_index;
	idx_t remaining = count;
	while (remaining > 0) {
		auto &current_segment = segment.GetVectorData(current_index);
		idx_t append_count = MinValue<idx_t>(STANDARD_VECTOR_SIZE - current_segment.count, remaining);

		auto base_ptr = meta_data.segment.allocator->GetDataPointer(append_state.current_chunk_state,
		                                                            current_segment.block_id, current_segment.offset);
		auto validity_data = ColumnDataCollectionSegment::GetValidityPointer(base_ptr, OP::TypeSize());

		ValidityMask result_validity(validity_data);
		if (current_segment.count == 0) {
			// first time appending to this vector
			// all data here is still uninitialized
			// initialize the validity mask to set all to valid
			result_validity.SetAllValid(STANDARD_VECTOR_SIZE);
		}
		for (idx_t i = 0; i < append_count; i++) {
			auto source_idx = source_data.sel->get_index(offset + i);
			if (source_data.validity.RowIsValid(source_idx)) {
				OP::template Assign<OP>(meta_data, base_ptr, source_data.data, current_segment.count + i, source_idx);
			} else {
				result_validity.SetInvalid(current_segment.count + i);
			}
		}
		current_segment.count += append_count;
		offset += append_count;
		remaining -= append_count;
		if (remaining > 0) {
			// need to append more, check if we need to allocate a new vector or not
			if (!current_segment.next_data.IsValid()) {
				segment.AllocateVector(source.GetType(), meta_data.chunk_data, meta_data.state, current_index);
			}
			D_ASSERT(segment.GetVectorData(current_index).next_data.IsValid());
			current_index = segment.GetVectorData(current_index).next_data;
		}
	}
}

template <class T>
static void ColumnDataCopy(ColumnDataMetaData &meta_data, const UnifiedVectorFormat &source_data, Vector &source,
                           idx_t offset, idx_t copy_count) {
	TemplatedColumnDataCopy<StandardValueCopy<T>>(meta_data, source_data, source, offset, copy_count);
}

template <>
void ColumnDataCopy<string_t>(ColumnDataMetaData &meta_data, const UnifiedVectorFormat &source_data, Vector &source,
                              idx_t offset, idx_t copy_count) {
	TemplatedColumnDataCopy<StringValueCopy>(meta_data, source_data, source, offset, copy_count);
}

template <>
void ColumnDataCopy<list_entry_t>(ColumnDataMetaData &meta_data, const UnifiedVectorFormat &source_data, Vector &source,
                                  idx_t offset, idx_t copy_count) {
	auto &segment = meta_data.segment;

	// first append the child entries of the list
	auto &child_vector = ListVector::GetEntry(source);
	idx_t child_list_size = ListVector::GetListSize(source);
	auto &child_type = child_vector.GetType();

	UnifiedVectorFormat child_vector_data;
	child_vector.ToUnifiedFormat(child_list_size, child_vector_data);

	if (!meta_data.GetVectorMetaData().child_index.IsValid()) {
		auto child_index = segment.AllocateVector(child_type, meta_data.chunk_data, meta_data.state);
		meta_data.GetVectorMetaData().child_index = meta_data.segment.AddChildIndex(child_index);
	}
	auto &child_function = meta_data.copy_function.child_functions[0];
	auto child_index = segment.GetChildIndex(meta_data.GetVectorMetaData().child_index);
	// figure out the current list size by traversing the set of child entries
	idx_t current_list_size = 0;
	auto current_child_index = child_index;
	while (current_child_index.IsValid()) {
		auto &child_vdata = segment.GetVectorData(current_child_index);
		current_list_size += child_vdata.count;
		current_child_index = child_vdata.next_data;
	}
	ColumnDataMetaData child_meta_data(child_function, meta_data, child_index);
	// FIXME: appending the entire child list here is not required
	// We can also scan the actual list entries required per the offset/copy_count
	child_function.function(child_meta_data, child_vector_data, child_vector, 0, child_list_size);

	// now copy the list entries
	meta_data.child_list_size = current_list_size;
	TemplatedColumnDataCopy<ListValueCopy>(meta_data, source_data, source, offset, copy_count);
}

void ColumnDataCopyStruct(ColumnDataMetaData &meta_data, const UnifiedVectorFormat &source_data, Vector &source,
                          idx_t offset, idx_t copy_count) {
	auto &segment = meta_data.segment;

	// copy the NULL values for the main struct vector
	TemplatedColumnDataCopy<StructValueCopy>(meta_data, source_data, source, offset, copy_count);

	auto &child_types = StructType::GetChildTypes(source.GetType());
	// now copy all the child vectors
	D_ASSERT(meta_data.GetVectorMetaData().child_index.IsValid());
	auto &child_vectors = StructVector::GetEntries(source);
	for (idx_t child_idx = 0; child_idx < child_types.size(); child_idx++) {
		auto &child_function = meta_data.copy_function.child_functions[child_idx];
		auto child_index = segment.GetChildIndex(meta_data.GetVectorMetaData().child_index, child_idx);
		ColumnDataMetaData child_meta_data(child_function, meta_data, child_index);

		UnifiedVectorFormat child_data;
		child_vectors[child_idx]->ToUnifiedFormat(copy_count, child_data);

		child_function.function(child_meta_data, child_data, *child_vectors[child_idx], offset, copy_count);
	}
}

ColumnDataCopyFunction ColumnDataCollection::GetCopyFunction(const LogicalType &type) {
	ColumnDataCopyFunction result;
	column_data_copy_function_t function;
	switch (type.InternalType()) {
	case PhysicalType::BOOL:
		function = ColumnDataCopy<bool>;
		break;
	case PhysicalType::INT8:
		function = ColumnDataCopy<int8_t>;
		break;
	case PhysicalType::INT16:
		function = ColumnDataCopy<int16_t>;
		break;
	case PhysicalType::INT32:
		function = ColumnDataCopy<int32_t>;
		break;
	case PhysicalType::INT64:
		function = ColumnDataCopy<int64_t>;
		break;
	case PhysicalType::INT128:
		function = ColumnDataCopy<hugeint_t>;
		break;
	case PhysicalType::UINT8:
		function = ColumnDataCopy<uint8_t>;
		break;
	case PhysicalType::UINT16:
		function = ColumnDataCopy<uint16_t>;
		break;
	case PhysicalType::UINT32:
		function = ColumnDataCopy<uint32_t>;
		break;
	case PhysicalType::UINT64:
		function = ColumnDataCopy<uint64_t>;
		break;
	case PhysicalType::FLOAT:
		function = ColumnDataCopy<float>;
		break;
	case PhysicalType::DOUBLE:
		function = ColumnDataCopy<double>;
		break;
	case PhysicalType::INTERVAL:
		function = ColumnDataCopy<interval_t>;
		break;
	case PhysicalType::VARCHAR:
		function = ColumnDataCopy<string_t>;
		break;
	case PhysicalType::STRUCT: {
		function = ColumnDataCopyStruct;
		auto &child_types = StructType::GetChildTypes(type);
		for (auto &kv : child_types) {
			result.child_functions.push_back(GetCopyFunction(kv.second));
		}
		break;
	}
	case PhysicalType::LIST: {
		function = ColumnDataCopy<list_entry_t>;
		auto child_function = GetCopyFunction(ListType::GetChildType(type));
		result.child_functions.push_back(child_function);
		break;
	}
	default:
		throw InternalException("Unsupported type for ColumnDataCollection::GetCopyFunction");
	}
	result.function = function;
	return result;
}

static bool IsComplexType(const LogicalType &type) {
	switch (type.InternalType()) {
	case PhysicalType::STRUCT:
	case PhysicalType::LIST:
		return true;
	default:
		return false;
	};
}

void ColumnDataCollection::Append(ColumnDataAppendState &state, DataChunk &input) {
	D_ASSERT(!finished_append);
	D_ASSERT(types == input.GetTypes());

	auto &segment = *segments.back();
	for (idx_t vector_idx = 0; vector_idx < types.size(); vector_idx++) {
		if (IsComplexType(input.data[vector_idx].GetType())) {
			input.data[vector_idx].Flatten(input.size());
		}
		input.data[vector_idx].ToUnifiedFormat(input.size(), state.vector_data[vector_idx]);
	}

	idx_t remaining = input.size();
	while (remaining > 0) {
		auto &chunk_data = segment.chunk_data.back();
		idx_t append_amount = MinValue<idx_t>(remaining, STANDARD_VECTOR_SIZE - chunk_data.count);
		if (append_amount > 0) {
			idx_t offset = input.size() - remaining;
			for (idx_t vector_idx = 0; vector_idx < types.size(); vector_idx++) {
				ColumnDataMetaData meta_data(copy_functions[vector_idx], segment, state, chunk_data,
				                             chunk_data.vector_data[vector_idx]);
				copy_functions[vector_idx].function(meta_data, state.vector_data[vector_idx], input.data[vector_idx],
				                                    offset, append_amount);
			}
			chunk_data.count += append_amount;
		}
		remaining -= append_amount;
		if (remaining > 0) {
			// more to do
			// allocate a new chunk
			segment.AllocateNewChunk();
			segment.InitializeChunkState(segment.chunk_data.size() - 1, state.current_chunk_state);
		}
	}
	segment.count += input.size();
	count += input.size();
}

void ColumnDataCollection::Append(DataChunk &input) {
	ColumnDataAppendState state;
	InitializeAppend(state);
	Append(state, input);
}

//===--------------------------------------------------------------------===//
// Scan
//===--------------------------------------------------------------------===//
void ColumnDataCollection::InitializeScan(ColumnDataScanState &state, ColumnDataScanProperties properties) const {
	vector<column_t> column_ids;
	column_ids.reserve(types.size());
	for (idx_t i = 0; i < types.size(); i++) {
		column_ids.push_back(i);
	}
	InitializeScan(state, move(column_ids), properties);
}

void ColumnDataCollection::InitializeScan(ColumnDataScanState &state, vector<column_t> column_ids,
                                          ColumnDataScanProperties properties) const {
	state.chunk_index = 0;
	state.segment_index = 0;
	state.current_row_index = 0;
	state.next_row_index = 0;
	state.current_chunk_state.handles.clear();
	state.properties = properties;
	state.column_ids = move(column_ids);
}

void ColumnDataCollection::InitializeScan(ColumnDataParallelScanState &state,
                                          ColumnDataScanProperties properties) const {
	InitializeScan(state.scan_state, properties);
}

void ColumnDataCollection::InitializeScan(ColumnDataParallelScanState &state, vector<column_t> column_ids,
                                          ColumnDataScanProperties properties) const {
	InitializeScan(state.scan_state, move(column_ids), properties);
}

bool ColumnDataCollection::Scan(ColumnDataParallelScanState &state, ColumnDataLocalScanState &lstate,
                                DataChunk &result) const {
	result.Reset();

	idx_t chunk_index;
	idx_t segment_index;
	idx_t row_index;
	{
		lock_guard<mutex> l(state.lock);
		if (!NextScanIndex(state.scan_state, chunk_index, segment_index, row_index)) {
			return false;
		}
	}
	ScanAtIndex(state, lstate, result, chunk_index, segment_index, row_index);
	return true;
}

void ColumnDataCollection::InitializeScanChunk(DataChunk &chunk) const {
	chunk.Initialize(allocator->GetAllocator(), types);
}

void ColumnDataCollection::InitializeScanChunk(ColumnDataScanState &state, DataChunk &chunk) const {
	D_ASSERT(!state.column_ids.empty());
	vector<LogicalType> chunk_types;
	chunk_types.reserve(state.column_ids.size());
	for (idx_t i = 0; i < state.column_ids.size(); i++) {
		auto column_idx = state.column_ids[i];
		D_ASSERT(column_idx < types.size());
		chunk_types.push_back(types[column_idx]);
	}
	chunk.Initialize(allocator->GetAllocator(), chunk_types);
}

bool ColumnDataCollection::NextScanIndex(ColumnDataScanState &state, idx_t &chunk_index, idx_t &segment_index,
                                         idx_t &row_index) const {
	row_index = state.current_row_index = state.next_row_index;
	// check if we still have collections to scan
	if (state.segment_index >= segments.size()) {
		// no more data left in the scan
		return false;
	}
	// check within the current collection if we still have chunks to scan
	while (state.chunk_index >= segments[state.segment_index]->chunk_data.size()) {
		// exhausted all chunks for this internal data structure: move to the next one
		state.chunk_index = 0;
		state.segment_index++;
		state.current_chunk_state.handles.clear();
		if (state.segment_index >= segments.size()) {
			return false;
		}
	}
	state.next_row_index += segments[state.segment_index]->chunk_data[state.chunk_index].count;
	segment_index = state.segment_index;
	chunk_index = state.chunk_index++;
	return true;
}

void ColumnDataCollection::ScanAtIndex(ColumnDataParallelScanState &state, ColumnDataLocalScanState &lstate,
                                       DataChunk &result, idx_t chunk_index, idx_t segment_index,
                                       idx_t row_index) const {
	if (segment_index != lstate.current_segment_index) {
		lstate.current_chunk_state.handles.clear();
		lstate.current_segment_index = segment_index;
	}
	auto &segment = *segments[segment_index];
	lstate.current_chunk_state.properties = state.scan_state.properties;
	segment.ReadChunk(chunk_index, lstate.current_chunk_state, result, state.scan_state.column_ids);
	lstate.current_row_index = row_index;
	result.Verify();
}

bool ColumnDataCollection::Scan(ColumnDataScanState &state, DataChunk &result) const {
	result.Reset();

	idx_t chunk_index;
	idx_t segment_index;
	idx_t row_index;
	if (!NextScanIndex(state, chunk_index, segment_index, row_index)) {
		return false;
	}

	// found a chunk to scan -> scan it
	auto &segment = *segments[segment_index];
	state.current_chunk_state.properties = state.properties;
	segment.ReadChunk(chunk_index, state.current_chunk_state, result, state.column_ids);
	result.Verify();
	return true;
}

ColumnDataRowCollection ColumnDataCollection::GetRows() const {
	return ColumnDataRowCollection(*this);
}

//===--------------------------------------------------------------------===//
// Combine
//===--------------------------------------------------------------------===//
void ColumnDataCollection::Combine(ColumnDataCollection &other) {
	if (other.count == 0) {
		return;
	}
	if (types != other.types) {
		throw InternalException("Attempting to combine ColumnDataCollections with mismatching types");
	}
	this->count += other.count;
	this->segments.reserve(segments.size() + other.segments.size());
	for (auto &other_seg : other.segments) {
		segments.push_back(move(other_seg));
	}
	Verify();
}

//===--------------------------------------------------------------------===//
// Fetch
//===--------------------------------------------------------------------===//
idx_t ColumnDataCollection::ChunkCount() const {
	idx_t chunk_count = 0;
	for (auto &segment : segments) {
		chunk_count += segment->ChunkCount();
	}
	return chunk_count;
}

void ColumnDataCollection::FetchChunk(idx_t chunk_idx, DataChunk &result) const {
	D_ASSERT(chunk_idx < ChunkCount());
	for (auto &segment : segments) {
		if (chunk_idx >= segment->ChunkCount()) {
			chunk_idx -= segment->ChunkCount();
		} else {
			segment->FetchChunk(chunk_idx, result);
			return;
		}
	}
	throw InternalException("Failed to find chunk in ColumnDataCollection");
}

//===--------------------------------------------------------------------===//
// Helpers
//===--------------------------------------------------------------------===//
void ColumnDataCollection::Verify() {
#ifdef DEBUG
	// verify counts
	idx_t total_segment_count = 0;
	for (auto &segment : segments) {
		segment->Verify();
		total_segment_count += segment->count;
	}
	D_ASSERT(total_segment_count == this->count);
#endif
}

string ColumnDataCollection::ToString() const {
	return "Column Data Collection";
}

void ColumnDataCollection::Print() const {
	Printer::Print(ToString());
}

void ColumnDataCollection::Reset() {
	count = 0;
	segments.clear();
}

bool ColumnDataCollection::ResultEquals(const ColumnDataCollection &left, const ColumnDataCollection &right,
                                        string &error_message) {
	if (left.ColumnCount() != right.ColumnCount()) {
		error_message = "Column count mismatch";
		return false;
	}
	if (left.Count() != right.Count()) {
		error_message = "Row count mismatch";
		return false;
	}
	auto left_rows = left.GetRows();
	auto right_rows = right.GetRows();
	for (idx_t r = 0; r < left.Count(); r++) {
		for (idx_t c = 0; c < left.ColumnCount(); c++) {
			auto lvalue = left_rows.GetValue(c, r);
			auto rvalue = left_rows.GetValue(c, r);
			if (!Value::DefaultValuesAreEqual(lvalue, rvalue)) {
				error_message =
				    StringUtil::Format("%s <> %s (row: %lld, col: %lld)\n", lvalue.ToString(), rvalue.ToString(), r, c);
				return false;
			}
		}
	}
	return true;
}

} // namespace duckdb
