#include "duckdb/common/operator/add.hpp"
#include "duckdb/common/operator/subtract.hpp"
#include "duckdb/function/window/window_boundaries_state.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// WindowBoundariesState
//===--------------------------------------------------------------------===//
idx_t WindowBoundariesState::FindNextStart(const ValidityMask &mask, idx_t l, const idx_t r, idx_t &n) {
	if (mask.AllValid()) {
		auto start = MinValue(l + n - 1, r);
		n -= MinValue(n, r - l);
		return start;
	}

	while (l < r) {
		//	If l is aligned with the start of a block, and the block is blank, then skip forward one block.
		idx_t entry_idx;
		idx_t shift;
		mask.GetEntryIndex(l, entry_idx, shift);

		const auto block = mask.GetValidityEntry(entry_idx);
		if (mask.NoneValid(block) && !shift) {
			l += ValidityMask::BITS_PER_VALUE;
			continue;
		}

		// Loop over the block
		for (; shift < ValidityMask::BITS_PER_VALUE && l < r; ++shift, ++l) {
			if (mask.RowIsValid(block, shift) && --n == 0) {
				return MinValue(l, r);
			}
		}
	}

	//	Didn't find a start so return the end of the range
	return r;
}

idx_t WindowBoundariesState::FindPrevStart(const ValidityMask &mask, const idx_t l, idx_t r, idx_t &n) {
	if (mask.AllValid()) {
		auto start = (r <= l + n) ? l : r - n;
		n -= r - start;
		return start;
	}

	while (l < r) {
		// If r is aligned with the start of a block, and the previous block is blank,
		// then skip backwards one block.
		idx_t entry_idx;
		idx_t shift;
		mask.GetEntryIndex(r - 1, entry_idx, shift);

		const auto block = mask.GetValidityEntry(entry_idx);
		if (mask.NoneValid(block) && (shift + 1 == ValidityMask::BITS_PER_VALUE)) {
			// r is nonzero (> l) and word aligned, so this will not underflow.
			r -= ValidityMask::BITS_PER_VALUE;
			continue;
		}

		// Loop backwards over the block
		// shift is probing r-1 >= l >= 0
		for (++shift; shift-- > 0 && l < r; --r) {
			// l < r ensures n == 1 if result is supposed to be NULL because of EXCLUDE
			if (mask.RowIsValid(block, shift) && --n == 0) {
				return MaxValue(l, r - 1);
			}
		}
	}

	//	Didn't find a start so return the start of the range
	return l;
}

//===--------------------------------------------------------------------===//
// WindowColumnIterator
//===--------------------------------------------------------------------===//
template <typename T>
struct WindowColumnIterator {
	using iterator = WindowColumnIterator<T>;
	using iterator_category = std::random_access_iterator_tag;
	using difference_type = std::ptrdiff_t;
	using value_type = T;
	using reference = T;
	using pointer = idx_t;

	explicit WindowColumnIterator(WindowCursor &coll, pointer pos = 0) : coll(&coll), pos(pos) {
	}

	//	Forward iterator
	inline reference operator*() const {
		return coll->GetCell<T>(0, pos);
	}
	inline explicit operator pointer() const {
		return pos;
	}

	inline iterator &operator++() {
		++pos;
		return *this;
	}
	inline iterator operator++(int) {
		auto result = *this;
		++(*this);
		return result;
	}

	//	Bidirectional iterator
	inline iterator &operator--() {
		--pos;
		return *this;
	}
	inline iterator operator--(int) {
		auto result = *this;
		--(*this);
		return result;
	}

	//	Random Access
	inline iterator &operator+=(difference_type n) {
		pos += UnsafeNumericCast<pointer>(n);
		return *this;
	}
	inline iterator &operator-=(difference_type n) {
		pos -= UnsafeNumericCast<pointer>(n);
		return *this;
	}

	inline reference operator[](difference_type m) const {
		return coll->GetCell<T>(0, pos + m);
	}

	friend inline iterator &operator+(const iterator &a, difference_type n) {
		return iterator(a.coll, a.pos + n);
	}

	friend inline iterator operator-(const iterator &a, difference_type n) {
		return iterator(a.coll, a.pos - n);
	}

	friend inline iterator operator+(difference_type n, const iterator &a) {
		return a + n;
	}
	friend inline difference_type operator-(const iterator &a, const iterator &b) {
		return difference_type(a.pos - b.pos);
	}

	friend inline bool operator==(const iterator &a, const iterator &b) {
		return a.pos == b.pos;
	}
	friend inline bool operator!=(const iterator &a, const iterator &b) {
		return a.pos != b.pos;
	}
	friend inline bool operator<(const iterator &a, const iterator &b) {
		return a.pos < b.pos;
	}
	friend inline bool operator<=(const iterator &a, const iterator &b) {
		return a.pos <= b.pos;
	}
	friend inline bool operator>(const iterator &a, const iterator &b) {
		return a.pos > b.pos;
	}
	friend inline bool operator>=(const iterator &a, const iterator &b) {
		return a.pos >= b.pos;
	}

private:
	// optional_ptr does not allow us to modify this, but the constructor enforces it.
	WindowCursor *coll;
	pointer pos;
};

template <typename T, typename OP>
struct OperationCompare : public std::function<bool(T, T)> {
	inline bool operator()(const T &lhs, const T &val) const {
		return OP::template Operation<T>(lhs, val);
	}
};

template <typename T, typename OP, bool FROM>
static idx_t FindTypedRangeBound(WindowCursor &range_lo, WindowCursor &range_hi, const idx_t order_begin,
                                 const idx_t order_end, const WindowBoundary range, WindowInputExpression &boundary,
                                 const idx_t chunk_idx, const FrameBounds &prev) {
	D_ASSERT(!boundary.CellIsNull(chunk_idx));
	const auto val = boundary.GetCell<T>(chunk_idx);

	OperationCompare<T, OP> comp;

	// Check that the value we are searching for is in range.
	if (range == WindowBoundary::EXPR_PRECEDING_RANGE) {
		//	Preceding but value past the current value
		const auto cur_val = range_hi.GetCell<T>(0, order_end - 1);
		if (comp(cur_val, val)) {
			throw OutOfRangeException("Invalid RANGE PRECEDING value");
		}
	} else {
		//	Following but value before the current value
		D_ASSERT(range == WindowBoundary::EXPR_FOLLOWING_RANGE);
		const auto cur_val = range_lo.GetCell<T>(0, order_begin);
		if (comp(val, cur_val)) {
			throw OutOfRangeException("Invalid RANGE FOLLOWING value");
		}
	}
	//	Try to reuse the previous bounds to restrict the search.
	//	This is only valid if the previous bounds were non-empty
	WindowColumnIterator<T> begin(range_lo, order_begin);
	WindowColumnIterator<T> end(range_hi, order_end);
	if (prev.start < prev.end) {
		if (order_begin <= prev.start && prev.start < order_end) {
			const auto first = range_lo.GetCell<T>(0, prev.start);
			if (FROM && !comp(val, first)) {
				// If prev.start == val and we are looking for a lower bound, then we are done
				if (!comp(first, val)) {
					return prev.start;
				}
				//	prev.start <= val, so we can start further forward
				begin += UnsafeNumericCast<int64_t>(prev.start - order_begin);
			}
		}
		if (order_begin < prev.end && prev.end < order_end) {
			const auto second = range_hi.GetCell<T>(0, prev.end - 1);
			if (!comp(second, val)) {
				//  If val == prev.end and we are looking for an upper bound, then we are done
				if (!FROM && !comp(val, second)) {
					return prev.end;
				}
				//	val <= prev.end, so we can end further back
				// (prev.second is the largest peer)
				end -= UnsafeNumericCast<int64_t>(order_end - prev.end - 1);
			}
		}
	}

	if (FROM) {
		return idx_t(std::lower_bound(begin, end, val, comp));
	} else {
		return idx_t(std::upper_bound(begin, end, val, comp));
	}
}

template <typename OP, bool FROM>
static idx_t FindRangeBound(WindowCursor &range_lo, WindowCursor &range_hi, const idx_t order_begin,
                            const idx_t order_end, const WindowBoundary range, WindowInputExpression &boundary,
                            const idx_t chunk_idx, const FrameBounds &prev) {
	switch (boundary.InternalType()) {
	case PhysicalType::INT8:
		return FindTypedRangeBound<int8_t, OP, FROM>(range_lo, range_hi, order_begin, order_end, range, boundary,
		                                             chunk_idx, prev);
	case PhysicalType::INT16:
		return FindTypedRangeBound<int16_t, OP, FROM>(range_lo, range_hi, order_begin, order_end, range, boundary,
		                                              chunk_idx, prev);
	case PhysicalType::INT32:
		return FindTypedRangeBound<int32_t, OP, FROM>(range_lo, range_hi, order_begin, order_end, range, boundary,
		                                              chunk_idx, prev);
	case PhysicalType::INT64:
		return FindTypedRangeBound<int64_t, OP, FROM>(range_lo, range_hi, order_begin, order_end, range, boundary,
		                                              chunk_idx, prev);
	case PhysicalType::UINT8:
		return FindTypedRangeBound<uint8_t, OP, FROM>(range_lo, range_hi, order_begin, order_end, range, boundary,
		                                              chunk_idx, prev);
	case PhysicalType::UINT16:
		return FindTypedRangeBound<uint16_t, OP, FROM>(range_lo, range_hi, order_begin, order_end, range, boundary,
		                                               chunk_idx, prev);
	case PhysicalType::UINT32:
		return FindTypedRangeBound<uint32_t, OP, FROM>(range_lo, range_hi, order_begin, order_end, range, boundary,
		                                               chunk_idx, prev);
	case PhysicalType::UINT64:
		return FindTypedRangeBound<uint64_t, OP, FROM>(range_lo, range_hi, order_begin, order_end, range, boundary,
		                                               chunk_idx, prev);
	case PhysicalType::INT128:
		return FindTypedRangeBound<hugeint_t, OP, FROM>(range_lo, range_hi, order_begin, order_end, range, boundary,
		                                                chunk_idx, prev);
	case PhysicalType::UINT128:
		return FindTypedRangeBound<uhugeint_t, OP, FROM>(range_lo, range_hi, order_begin, order_end, range, boundary,
		                                                 chunk_idx, prev);
	case PhysicalType::FLOAT:
		return FindTypedRangeBound<float, OP, FROM>(range_lo, range_hi, order_begin, order_end, range, boundary,
		                                            chunk_idx, prev);
	case PhysicalType::DOUBLE:
		return FindTypedRangeBound<double, OP, FROM>(range_lo, range_hi, order_begin, order_end, range, boundary,
		                                             chunk_idx, prev);
	case PhysicalType::INTERVAL:
		return FindTypedRangeBound<interval_t, OP, FROM>(range_lo, range_hi, order_begin, order_end, range, boundary,
		                                                 chunk_idx, prev);
	default:
		throw InternalException("Unsupported column type for RANGE");
	}
}

template <bool FROM>
static idx_t FindOrderedRangeBound(WindowCursor &range_lo, WindowCursor &range_hi, const OrderType range_sense,
                                   const idx_t order_begin, const idx_t order_end, const WindowBoundary range,
                                   WindowInputExpression &boundary, const idx_t chunk_idx, const FrameBounds &prev) {
	switch (range_sense) {
	case OrderType::ASCENDING:
		return FindRangeBound<LessThan, FROM>(range_lo, range_hi, order_begin, order_end, range, boundary, chunk_idx,
		                                      prev);
	case OrderType::DESCENDING:
		return FindRangeBound<GreaterThan, FROM>(range_lo, range_hi, order_begin, order_end, range, boundary, chunk_idx,
		                                         prev);
	default:
		throw InternalException("Unsupported ORDER BY sense for RANGE");
	}
}

bool WindowBoundariesState::HasPrecedingRange(const BoundWindowExpression &wexpr) {
	return (wexpr.start == WindowBoundary::EXPR_PRECEDING_RANGE || wexpr.end == WindowBoundary::EXPR_PRECEDING_RANGE);
}

bool WindowBoundariesState::HasFollowingRange(const BoundWindowExpression &wexpr) {
	return (wexpr.start == WindowBoundary::EXPR_FOLLOWING_RANGE || wexpr.end == WindowBoundary::EXPR_FOLLOWING_RANGE);
}

WindowBoundsSet WindowBoundariesState::GetWindowBounds(const BoundWindowExpression &wexpr) {
	const auto partition_count = wexpr.partitions.size();
	const auto order_count = wexpr.orders.size();

	WindowBoundsSet result;
	switch (wexpr.GetExpressionType()) {
	case ExpressionType::WINDOW_ROW_NUMBER:
		if (wexpr.arg_orders.empty()) {
			result.insert(PARTITION_BEGIN);
		} else {
			// Secondary orders need to know where the frame is
			result.insert(FRAME_BEGIN);
			result.insert(FRAME_END);
		}
		break;
	case ExpressionType::WINDOW_NTILE:
		if (wexpr.arg_orders.empty()) {
			result.insert(PARTITION_BEGIN);
			result.insert(PARTITION_END);
		} else {
			// Secondary orders need to know where the frame is
			result.insert(FRAME_BEGIN);
			result.insert(FRAME_END);
		}
		break;
	case ExpressionType::WINDOW_RANK:
		if (wexpr.arg_orders.empty()) {
			result.insert(PARTITION_BEGIN);
			result.insert(PEER_BEGIN);
		} else {
			// Secondary orders need to know where the frame is
			result.insert(FRAME_BEGIN);
			result.insert(FRAME_END);
			result.insert(PEER_BEGIN);
		}
		break;
	case ExpressionType::WINDOW_RANK_DENSE:
		result.insert(PARTITION_BEGIN);
		result.insert(PEER_BEGIN);
		break;
	case ExpressionType::WINDOW_PERCENT_RANK:
		if (wexpr.arg_orders.empty()) {
			result.insert(PARTITION_BEGIN);
			result.insert(PARTITION_END);
			result.insert(PEER_BEGIN);
		} else {
			// Secondary orders need to know where the frame is
			result.insert(FRAME_BEGIN);
			result.insert(FRAME_END);
			result.insert(PEER_BEGIN);
		}
		break;
	case ExpressionType::WINDOW_CUME_DIST:
		if (wexpr.arg_orders.empty()) {
			result.insert(PARTITION_BEGIN);
			result.insert(PARTITION_END);
			result.insert(PEER_END);
		} else {
			// Secondary orders need to know where the frame is
			result.insert(FRAME_BEGIN);
			result.insert(FRAME_END);
			result.insert(PEER_END);
		}
		break;
	case ExpressionType::WINDOW_LEAD:
	case ExpressionType::WINDOW_LAG:
		if (wexpr.arg_orders.empty()) {
			result.insert(PARTITION_BEGIN);
			result.insert(PARTITION_END);
		} else {
			// Secondary orders need to know where the frame is
			result.insert(FRAME_BEGIN);
			result.insert(FRAME_END);
		}
		break;
	case ExpressionType::WINDOW_FILL:
		result.insert(FRAME_BEGIN);
		result.insert(FRAME_END);
		if (wexpr.arg_orders.empty()) {
			//	FILL uses the validity ranges to quickly eliminate indexes that can't be interpolated.
			//	This only works for non-secondary orderings
			result.insert(VALID_BEGIN);
			result.insert(VALID_END);
		}
		break;
	case ExpressionType::WINDOW_FIRST_VALUE:
	case ExpressionType::WINDOW_LAST_VALUE:
	case ExpressionType::WINDOW_NTH_VALUE:
	case ExpressionType::WINDOW_AGGREGATE:
		result.insert(FRAME_BEGIN);
		result.insert(FRAME_END);
		break;
	default:
		throw InternalException("Window expression type %s", ExpressionTypeToString(wexpr.GetExpressionType()));
	}

	//	Internal dependencies
	if (result.count(FRAME_BEGIN) || result.count(FRAME_END)) {
		result.insert(PARTITION_BEGIN);
		result.insert(PARTITION_END);

		// if we have EXCLUDE GROUP / TIES, we also need peer boundaries
		if (wexpr.exclude_clause != WindowExcludeMode::NO_OTHER) {
			result.insert(PEER_BEGIN);
			result.insert(PEER_END);
		}

		// If the frames are RANGE or GROUPS, then we need peer boundaries
		// If they are preceding or following, RANGE also needs to know
		// where the valid values begin or end.
		switch (wexpr.start) {
		case WindowBoundary::CURRENT_ROW_RANGE:
		case WindowBoundary::CURRENT_ROW_GROUPS:
			result.insert(PEER_BEGIN);
			break;
		case WindowBoundary::EXPR_PRECEDING_RANGE:
			result.insert(PEER_BEGIN);
			result.insert(VALID_BEGIN);
			result.insert(VALID_END);
			break;
		case WindowBoundary::EXPR_FOLLOWING_RANGE:
			result.insert(PEER_BEGIN);
			result.insert(VALID_END);
			break;
		case WindowBoundary::EXPR_PRECEDING_GROUPS:
			result.insert(PEER_BEGIN);
			break;
		case WindowBoundary::EXPR_FOLLOWING_GROUPS:
			result.insert(PEER_BEGIN);
			break;
		case WindowBoundary::UNBOUNDED_PRECEDING:
		case WindowBoundary::UNBOUNDED_FOLLOWING:
		case WindowBoundary::CURRENT_ROW_ROWS:
		case WindowBoundary::EXPR_PRECEDING_ROWS:
		case WindowBoundary::EXPR_FOLLOWING_ROWS:
		case WindowBoundary::INVALID:
			break;
		}

		switch (wexpr.end) {
		case WindowBoundary::CURRENT_ROW_RANGE:
		case WindowBoundary::CURRENT_ROW_GROUPS:
			result.insert(PEER_END);
			break;
		case WindowBoundary::EXPR_PRECEDING_RANGE:
			result.insert(PEER_END);
			result.insert(VALID_BEGIN);
			break;
		case WindowBoundary::EXPR_FOLLOWING_RANGE:
			result.insert(PEER_END);
			result.insert(VALID_BEGIN);
			result.insert(VALID_END);
			break;
		case WindowBoundary::EXPR_PRECEDING_GROUPS:
			result.insert(PEER_END);
			break;
		case WindowBoundary::EXPR_FOLLOWING_GROUPS:
			result.insert(PEER_END);
			break;
		case WindowBoundary::UNBOUNDED_PRECEDING:
		case WindowBoundary::UNBOUNDED_FOLLOWING:
		case WindowBoundary::CURRENT_ROW_ROWS:
		case WindowBoundary::EXPR_PRECEDING_ROWS:
		case WindowBoundary::EXPR_FOLLOWING_ROWS:
		case WindowBoundary::INVALID:
			break;
		}
	}

	if (result.count(VALID_END)) {
		result.insert(PARTITION_END);
		if (HasFollowingRange(wexpr)) {
			result.insert(VALID_BEGIN);
		}
	}
	if (result.count(VALID_BEGIN)) {
		result.insert(PARTITION_BEGIN);
		result.insert(PARTITION_END);
	}
	if (result.count(PEER_END)) {
		result.insert(PARTITION_END);
		if (order_count) {
			result.insert(PEER_BEGIN);
		}
	}
	if (result.count(PARTITION_END) && (partition_count + order_count)) {
		result.insert(PARTITION_BEGIN);
	}

	return result;
}

WindowBoundariesState::WindowBoundariesState(const BoundWindowExpression &wexpr, const idx_t input_size)
    : required(GetWindowBounds(wexpr)), type(wexpr.GetExpressionType()), input_size(input_size),
      start_boundary(wexpr.start), end_boundary(wexpr.end), partition_count(wexpr.partitions.size()),
      order_count(wexpr.orders.size()), range_sense(wexpr.orders.empty() ? OrderType::INVALID : wexpr.orders[0].type),
      has_preceding_range(HasPrecedingRange(wexpr)), has_following_range(HasFollowingRange(wexpr)) {
}

void WindowBoundariesState::Bounds(DataChunk &bounds, idx_t row_idx, optional_ptr<WindowCursor> range,
                                   const idx_t count, WindowInputExpression &boundary_start,
                                   WindowInputExpression &boundary_end, const ValidityMask &partition_mask,
                                   const ValidityMask &order_mask) {
	bounds.Reset();
	D_ASSERT(bounds.ColumnCount() == 8);

	//	Have we jumped from the previous position?
	const auto is_jump = (next_pos != row_idx);
	if (is_jump) {
		next_pos = row_idx;
	}

	if (required.count(PARTITION_BEGIN)) {
		PartitionBegin(bounds, row_idx, count, is_jump, partition_mask);
	}
	if (required.count(PARTITION_END)) {
		PartitionEnd(bounds, row_idx, count, is_jump, partition_mask);
	}
	if (required.count(PEER_BEGIN)) {
		PeerBegin(bounds, row_idx, count, is_jump, partition_mask, order_mask);
	}
	if (required.count(PEER_END)) {
		PeerEnd(bounds, row_idx, count, partition_mask, order_mask);
	}
	if (required.count(VALID_BEGIN)) {
		ValidBegin(bounds, row_idx, count, is_jump, partition_mask, order_mask, range);
	}
	if (required.count(VALID_END)) {
		ValidEnd(bounds, row_idx, count, is_jump, partition_mask, order_mask, range);
	}
	if (required.count(FRAME_BEGIN)) {
		FrameBegin(bounds, row_idx, count, boundary_start, order_mask, range);
	}
	if (required.count(FRAME_END)) {
		FrameEnd(bounds, row_idx, count, boundary_end, order_mask, range);
	}
	next_pos += count;

	bounds.SetCardinality(count);
}

void WindowBoundariesState::PartitionBegin(DataChunk &bounds, idx_t row_idx, const idx_t count, bool is_jump,
                                           const ValidityMask &partition_mask) {
	auto partition_begin_data = FlatVector::GetData<idx_t>(bounds.data[PARTITION_BEGIN]);

	//	OVER()
	if (partition_count + order_count == 0) {
		for (idx_t chunk_idx = 0; chunk_idx < count; ++chunk_idx, ++row_idx) {
			partition_begin_data[chunk_idx] = 0;
		}
		return;
	}

	for (idx_t chunk_idx = 0; chunk_idx < count; ++chunk_idx, ++row_idx) {
		// determine partition and peer group boundaries to ultimately figure out window size
		const auto is_same_partition = !partition_mask.RowIsValidUnsafe(row_idx);

		// when the partition changes, recompute the boundaries
		if (!is_same_partition || is_jump) {
			if (is_jump) {
				idx_t n = 1;
				partition_start = FindPrevStart(partition_mask, 0, row_idx + 1, n);
				is_jump = false;
			} else {
				partition_start = row_idx;
			}
		}

		partition_begin_data[chunk_idx] = partition_start;
	}
}

void WindowBoundariesState::PartitionEnd(DataChunk &bounds, idx_t row_idx, const idx_t count, bool is_jump,
                                         const ValidityMask &partition_mask) {
	auto partition_end_data = FlatVector::GetData<idx_t>(bounds.data[PARTITION_END]);

	//	OVER()
	if (partition_count + order_count == 0) {
		for (idx_t chunk_idx = 0; chunk_idx < count; ++chunk_idx, ++row_idx) {
			partition_end_data[chunk_idx] = input_size;
		}
		return;
	}

	auto partition_begin_data = FlatVector::GetData<const idx_t>(bounds.data[PARTITION_BEGIN]);
	for (idx_t chunk_idx = 0; chunk_idx < count; ++chunk_idx, ++row_idx) {
		// determine partition and peer group boundaries to ultimately figure out window size
		const auto is_same_partition = !partition_mask.RowIsValidUnsafe(row_idx);

		// when the partition changes, recompute the boundaries
		if (!is_same_partition || is_jump) {
			// find end of partition
			partition_end = input_size;
			if (partition_count) {
				const auto partition_begin = partition_begin_data[chunk_idx];
				idx_t n = 1;
				partition_end = FindNextStart(partition_mask, partition_begin + 1, input_size, n);
			}
			is_jump = false;
		}

		partition_end_data[chunk_idx] = partition_end;
	}
}

void WindowBoundariesState::PeerBegin(DataChunk &bounds, idx_t row_idx, const idx_t count, bool is_jump,
                                      const ValidityMask &partition_mask, const ValidityMask &order_mask) {

	auto peer_begin_data = FlatVector::GetData<idx_t>(bounds.data[PEER_BEGIN]);

	//	OVER()
	if (partition_count + order_count == 0) {
		for (idx_t chunk_idx = 0; chunk_idx < count; ++chunk_idx, ++row_idx) {
			peer_begin_data[chunk_idx] = 0;
		}
		return;
	}

	for (idx_t chunk_idx = 0; chunk_idx < count; ++chunk_idx, ++row_idx) {
		// determine partition and peer group boundaries to ultimately figure out window size
		const auto is_same_partition = !partition_mask.RowIsValidUnsafe(row_idx);
		const auto is_peer = !order_mask.RowIsValidUnsafe(row_idx);

		// when the partition changes, recompute the boundaries
		if (!is_same_partition || is_jump) {
			// find end of partition
			if (is_jump) {
				idx_t n = 1;
				peer_start = FindPrevStart(order_mask, 0, row_idx + 1, n);
			} else {
				peer_start = row_idx;
			}
			is_jump = false;
		} else if (!is_peer) {
			peer_start = row_idx;
		}

		peer_begin_data[chunk_idx] = peer_start;
	}
}

void WindowBoundariesState::PeerEnd(DataChunk &bounds, idx_t row_idx, const idx_t count,
                                    const ValidityMask &partition_mask, const ValidityMask &order_mask) {
	//	OVER()
	if (!order_count) {
		bounds.data[PEER_END].Reference(bounds.data[PARTITION_END]);
		return;
	}

	auto partition_end_data = FlatVector::GetData<const idx_t>(bounds.data[PARTITION_END]);
	auto peer_begin_data = FlatVector::GetData<const idx_t>(bounds.data[PEER_BEGIN]);
	auto peer_end_data = FlatVector::GetData<idx_t>(bounds.data[PEER_END]);
	auto prev_end = peer_begin_data[0];
	for (idx_t chunk_idx = 0; chunk_idx < count; ++chunk_idx, ++row_idx) {
		const auto peer_start = peer_begin_data[chunk_idx];
		if (peer_start >= prev_end) {
			idx_t n = 1;
			const auto partition_end = partition_end_data[chunk_idx];
			prev_end = FindNextStart(order_mask, peer_start + 1, partition_end, n);
		}
		peer_end_data[chunk_idx] = prev_end;
	}
}

void WindowBoundariesState::ValidBegin(DataChunk &bounds, idx_t row_idx, const idx_t count, bool is_jump,
                                       const ValidityMask &partition_mask, const ValidityMask &order_mask,
                                       optional_ptr<WindowCursor> range) {
	auto partition_begin_data = FlatVector::GetData<const idx_t>(bounds.data[PARTITION_BEGIN]);
	auto partition_end_data = FlatVector::GetData<const idx_t>(bounds.data[PARTITION_END]);
	auto valid_begin_data = FlatVector::GetData<idx_t>(bounds.data[VALID_BEGIN]);

	//	OVER()
	D_ASSERT(partition_count + order_count != 0);
	D_ASSERT(range);

	for (idx_t chunk_idx = 0; chunk_idx < count; ++chunk_idx, ++row_idx) {
		const auto is_same_partition = !partition_mask.RowIsValidUnsafe(row_idx);

		if (!is_same_partition || is_jump) {
			// Find valid ordering values for the new partition
			// so we can exclude NULLs from RANGE expression computations
			valid_start = partition_begin_data[chunk_idx];
			const auto valid_end = partition_end_data[chunk_idx];

			if (valid_start < valid_end) {
				// Exclude any leading NULLs
				if (range->CellIsNull(0, valid_start)) {
					idx_t n = 1;
					valid_start = FindNextStart(order_mask, valid_start + 1, valid_end, n);
				}
			}
		}

		valid_begin_data[chunk_idx] = valid_start;
	}
}

void WindowBoundariesState::ValidEnd(DataChunk &bounds, idx_t row_idx, const idx_t count, bool is_jump,
                                     const ValidityMask &partition_mask, const ValidityMask &order_mask,
                                     optional_ptr<WindowCursor> range) {
	auto partition_end_data = FlatVector::GetData<const idx_t>(bounds.data[PARTITION_END]);
	auto valid_begin_data = FlatVector::GetData<const idx_t>(bounds.data[VALID_BEGIN]);
	auto valid_end_data = FlatVector::GetData<idx_t>(bounds.data[VALID_END]);

	//	OVER()
	D_ASSERT(partition_count + order_count != 0);
	D_ASSERT(range);

	for (idx_t chunk_idx = 0; chunk_idx < count; ++chunk_idx, ++row_idx) {
		const auto is_same_partition = !partition_mask.RowIsValidUnsafe(row_idx);

		if (!is_same_partition || is_jump) {
			// Find valid ordering values for the new partition
			// so we can exclude NULLs from RANGE expression computations
			const auto valid_start = valid_begin_data[chunk_idx];
			valid_end = partition_end_data[chunk_idx];

			if (valid_start < valid_end) {
				// Exclude any trailing NULLs
				if (range->CellIsNull(0, valid_end - 1)) {
					idx_t n = 1;
					valid_end = FindPrevStart(order_mask, valid_start, valid_end, n);
				}
			}
		}

		valid_end_data[chunk_idx] = valid_end;
	}
}

void WindowBoundariesState::FrameBegin(DataChunk &bounds, idx_t row_idx, const idx_t count,
                                       WindowInputExpression &boundary_begin, const ValidityMask &order_mask,
                                       optional_ptr<WindowCursor> range) {
	auto partition_begin_data = FlatVector::GetData<idx_t>(bounds.data[PARTITION_BEGIN]);
	auto partition_end_data = FlatVector::GetData<const idx_t>(bounds.data[PARTITION_END]);
	auto peer_begin_data = FlatVector::GetData<idx_t>(bounds.data[PEER_BEGIN]);
	auto valid_begin_data = FlatVector::GetData<const idx_t>(bounds.data[VALID_BEGIN]);
	auto valid_end_data = FlatVector::GetData<const idx_t>(bounds.data[VALID_END]);
	auto frame_begin_data = FlatVector::GetData<idx_t>(bounds.data[FRAME_BEGIN]);

	idx_t window_start = NumericLimits<idx_t>::Maximum();

	//	Reset previous range hints
	idx_t prev_partition = partition_begin_data[0];
	prev.start = valid_begin_data[0];
	prev.end = valid_end_data[0];

	if (has_preceding_range || has_following_range) {
		if (range_lo.get() != range.get()) {
			range_lo = range.get();
			range_hi = range_lo->Copy();
		}
	}

	switch (start_boundary) {
	case WindowBoundary::UNBOUNDED_PRECEDING:
		bounds.data[FRAME_BEGIN].Reference(bounds.data[PARTITION_BEGIN]);
		// No need to clamp
		return;
	case WindowBoundary::CURRENT_ROW_ROWS:
		for (idx_t chunk_idx = 0; chunk_idx < count; ++chunk_idx, ++row_idx) {
			frame_begin_data[chunk_idx] = row_idx;
		}
		break;
	case WindowBoundary::CURRENT_ROW_RANGE:
	case WindowBoundary::CURRENT_ROW_GROUPS:
		// in RANGE or GROUPS mode it means that the frame starts or ends with the current row's
		// first or last peer in the ORDER BY ordering
		bounds.data[FRAME_BEGIN].Reference(bounds.data[PEER_BEGIN]);
		frame_begin_data = peer_begin_data;
		break;
	case WindowBoundary::EXPR_PRECEDING_ROWS:
		for (idx_t chunk_idx = 0; chunk_idx < count; ++chunk_idx, ++row_idx) {
			int64_t computed_start;
			if (!TrySubtractOperator::Operation(static_cast<int64_t>(row_idx),
			                                    boundary_begin.GetCell<int64_t>(chunk_idx), computed_start)) {
				window_start = partition_begin_data[chunk_idx];
			} else {
				window_start = UnsafeNumericCast<idx_t>(MaxValue<int64_t>(computed_start, 0));
			}
			frame_begin_data[chunk_idx] = window_start;
		}
		break;
	case WindowBoundary::EXPR_FOLLOWING_ROWS:
		for (idx_t chunk_idx = 0; chunk_idx < count; ++chunk_idx, ++row_idx) {
			int64_t computed_start;
			if (!TryAddOperator::Operation(static_cast<int64_t>(row_idx), boundary_begin.GetCell<int64_t>(chunk_idx),
			                               computed_start)) {
				window_start = partition_begin_data[chunk_idx];
			} else {
				window_start = UnsafeNumericCast<idx_t>(MaxValue<int64_t>(computed_start, 0));
			}
			frame_begin_data[chunk_idx] = window_start;
		}
		break;
	case WindowBoundary::EXPR_PRECEDING_RANGE:
		for (idx_t chunk_idx = 0; chunk_idx < count; ++chunk_idx, ++row_idx) {
			if (boundary_begin.CellIsNull(chunk_idx)) {
				window_start = peer_begin_data[chunk_idx];
			} else {
				const auto valid_start = valid_begin_data[chunk_idx];
				prev.end = valid_end_data[chunk_idx];
				const auto cur_partition = partition_begin_data[chunk_idx];
				if (cur_partition != prev_partition) {
					prev.start = valid_start;
					prev_partition = cur_partition;
				}
				window_start = FindOrderedRangeBound<true>(*range_lo, *range_hi, range_sense, valid_start, row_idx + 1,
				                                           start_boundary, boundary_begin, chunk_idx, prev);
				prev.start = window_start;
			}
			frame_begin_data[chunk_idx] = window_start;
		}
		break;
	case WindowBoundary::EXPR_FOLLOWING_RANGE:
		for (idx_t chunk_idx = 0; chunk_idx < count; ++chunk_idx, ++row_idx) {
			if (boundary_begin.CellIsNull(chunk_idx)) {
				window_start = peer_begin_data[chunk_idx];
			} else {
				const auto valid_end = valid_end_data[chunk_idx];
				prev.end = valid_end;
				const auto cur_partition = partition_begin_data[chunk_idx];
				if (cur_partition != prev_partition) {
					prev.start = valid_begin_data[chunk_idx];
					prev_partition = cur_partition;
				}
				window_start = FindOrderedRangeBound<true>(*range_lo, *range_hi, range_sense, row_idx, valid_end,
				                                           start_boundary, boundary_begin, chunk_idx, prev);
				prev.start = window_start;
			}
			frame_begin_data[chunk_idx] = window_start;
		}
		break;
	case WindowBoundary::EXPR_PRECEDING_GROUPS:
		// In GROUPS mode, the offset is an integer indicating that the frame starts or ends that many peer groups
		// before or after the current row's peer group, where a peer group is a group of rows that are equivalent
		// according to the window's ORDER BY clause.
		for (idx_t chunk_idx = 0; chunk_idx < count; ++chunk_idx, ++row_idx) {
			if (boundary_begin.CellIsNull(chunk_idx)) {
				window_start = peer_begin_data[chunk_idx];
			} else {
				//	Count peer groups backwards.
				const auto peer_begin = peer_begin_data[chunk_idx];
				const auto partition_begin = partition_begin_data[chunk_idx];
				const auto boundary = boundary_begin.GetCell<int64_t>(chunk_idx);
				if (boundary < 0) {
					throw OutOfRangeException("Invalid GROUPS PRECEDING value");
				} else if (!boundary) {
					window_start = peer_begin;
				} else {
					auto n = UnsafeNumericCast<idx_t>(boundary);
					window_start = FindPrevStart(order_mask, partition_begin, peer_begin, n);
				}
			}
			frame_begin_data[chunk_idx] = window_start;
		}
		break;
	case WindowBoundary::EXPR_FOLLOWING_GROUPS:
		for (idx_t chunk_idx = 0; chunk_idx < count; ++chunk_idx, ++row_idx) {
			if (boundary_begin.CellIsNull(chunk_idx)) {
				window_start = peer_begin_data[chunk_idx];
			} else {
				//	Count peer groups forward.
				const auto peer_begin = peer_begin_data[chunk_idx];
				const auto partition_end = partition_end_data[chunk_idx];
				const auto boundary = boundary_begin.GetCell<int64_t>(chunk_idx);
				if (boundary < 0) {
					throw OutOfRangeException("Invalid GROUPS FOLLOWING value");
				} else if (!boundary) {
					window_start = peer_begin;
				} else {
					auto n = UnsafeNumericCast<idx_t>(boundary);
					window_start = FindNextStart(order_mask, peer_begin + 1, partition_end, n);
				}
			}
			frame_begin_data[chunk_idx] = window_start;
		}
		break;
	case WindowBoundary::UNBOUNDED_FOLLOWING:
	case WindowBoundary::INVALID:
		throw InternalException("Unsupported window start boundary");
	}

	ClampFrame(count, frame_begin_data, partition_begin_data, partition_end_data);
}

void WindowBoundariesState::FrameEnd(DataChunk &bounds, idx_t row_idx, const idx_t count,
                                     WindowInputExpression &boundary_end, const ValidityMask &order_mask,
                                     optional_ptr<WindowCursor> range) {
	auto partition_begin_data = FlatVector::GetData<const idx_t>(bounds.data[PARTITION_BEGIN]);
	auto partition_end_data = FlatVector::GetData<idx_t>(bounds.data[PARTITION_END]);
	auto peer_end_data = FlatVector::GetData<idx_t>(bounds.data[PEER_END]);
	auto valid_begin_data = FlatVector::GetData<const idx_t>(bounds.data[VALID_BEGIN]);
	auto valid_end_data = FlatVector::GetData<const idx_t>(bounds.data[VALID_END]);
	auto frame_end_data = FlatVector::GetData<idx_t>(bounds.data[FRAME_END]);

	idx_t window_end = NumericLimits<idx_t>::Maximum();

	//	Reset previous range hints
	idx_t prev_partition = partition_begin_data[0];
	prev.start = valid_begin_data[0];
	prev.end = valid_end_data[0];

	if (has_preceding_range || has_following_range) {
		if (range_lo.get() != range.get()) {
			range_lo = range.get();
			range_hi = range_lo->Copy();
		}
	}

	switch (end_boundary) {
	case WindowBoundary::CURRENT_ROW_ROWS:
		for (idx_t chunk_idx = 0; chunk_idx < count; ++chunk_idx, ++row_idx) {
			frame_end_data[chunk_idx] = row_idx + 1;
		}
		break;
	case WindowBoundary::CURRENT_ROW_RANGE:
	case WindowBoundary::CURRENT_ROW_GROUPS:
		// in RANGE or GROUPS mode it means that the frame starts or ends with the current row's
		// first or last peer in the ORDER BY ordering
		bounds.data[FRAME_END].Reference(bounds.data[PEER_END]);
		frame_end_data = peer_end_data;
		break;
	case WindowBoundary::UNBOUNDED_FOLLOWING:
		bounds.data[FRAME_END].Reference(bounds.data[PARTITION_END]);
		// No need to clamp
		return;
	case WindowBoundary::EXPR_PRECEDING_ROWS: {
		for (idx_t chunk_idx = 0; chunk_idx < count; ++chunk_idx, ++row_idx) {
			int64_t computed_start;
			if (!TrySubtractOperator::Operation(int64_t(row_idx + 1), boundary_end.GetCell<int64_t>(chunk_idx),
			                                    computed_start)) {
				window_end = partition_end_data[chunk_idx];
			} else {
				window_end = UnsafeNumericCast<idx_t>(MaxValue<int64_t>(computed_start, 0));
			}
			frame_end_data[chunk_idx] = window_end;
		}
		break;
	}
	case WindowBoundary::EXPR_FOLLOWING_ROWS:
		for (idx_t chunk_idx = 0; chunk_idx < count; ++chunk_idx, ++row_idx) {
			int64_t computed_start;
			if (!TryAddOperator::Operation(int64_t(row_idx + 1), boundary_end.GetCell<int64_t>(chunk_idx),
			                               computed_start)) {
				window_end = partition_end_data[chunk_idx];
			} else {
				window_end = UnsafeNumericCast<idx_t>(MaxValue<int64_t>(computed_start, 0));
			}
			frame_end_data[chunk_idx] = window_end;
		}
		break;
	case WindowBoundary::EXPR_PRECEDING_RANGE:
		for (idx_t chunk_idx = 0; chunk_idx < count; ++chunk_idx, ++row_idx) {
			if (boundary_end.CellIsNull(chunk_idx)) {
				window_end = peer_end_data[chunk_idx];
			} else {
				const auto valid_start = valid_begin_data[chunk_idx];
				prev.start = valid_start;
				const auto cur_partition = partition_begin_data[chunk_idx];
				if (cur_partition != prev_partition) {
					prev.end = valid_end;
					prev_partition = cur_partition;
				}
				window_end = FindOrderedRangeBound<false>(*range_lo, *range_hi, range_sense, valid_start, row_idx + 1,
				                                          end_boundary, boundary_end, chunk_idx, prev);
				prev.end = window_end;
			}
			frame_end_data[chunk_idx] = window_end;
		}
		break;
	case WindowBoundary::EXPR_FOLLOWING_RANGE:
		for (idx_t chunk_idx = 0; chunk_idx < count; ++chunk_idx, ++row_idx) {
			if (boundary_end.CellIsNull(chunk_idx)) {
				window_end = peer_end_data[chunk_idx];
			} else {
				const auto valid_end = valid_end_data[chunk_idx];
				prev.start = valid_begin_data[chunk_idx];
				const auto cur_partition = partition_begin_data[chunk_idx];
				if (cur_partition != prev_partition) {
					prev.end = valid_end;
					prev_partition = cur_partition;
				}
				window_end = FindOrderedRangeBound<false>(*range_lo, *range_hi, range_sense, row_idx, valid_end,
				                                          end_boundary, boundary_end, chunk_idx, prev);
				prev.end = window_end;
			}
			frame_end_data[chunk_idx] = window_end;
		}
		break;
	case WindowBoundary::EXPR_PRECEDING_GROUPS:
		// In GROUPS mode, the offset is an integer indicating that the frame starts or ends that many peer groups
		// before or after the current row's peer group, where a peer group is a group of rows that are equivalent
		// according to the window's ORDER BY clause.
		for (idx_t chunk_idx = 0; chunk_idx < count; ++chunk_idx, ++row_idx) {
			if (boundary_end.CellIsNull(chunk_idx)) {
				window_end = peer_end_data[chunk_idx];
			} else {
				//	Count peer groups backwards.
				const auto peer_end = peer_end_data[chunk_idx];
				const auto partition_begin = partition_begin_data[chunk_idx];
				const auto boundary = boundary_end.GetCell<int64_t>(chunk_idx);
				if (boundary < 0) {
					throw OutOfRangeException("Invalid GROUPS PRECEDING value");
				} else if (!boundary) {
					window_end = peer_end;
				} else {
					auto n = UnsafeNumericCast<idx_t>(boundary);
					window_end = FindPrevStart(order_mask, partition_begin, peer_end, n);
				}
			}
			frame_end_data[chunk_idx] = window_end;
		}
		break;
	case WindowBoundary::EXPR_FOLLOWING_GROUPS:
		for (idx_t chunk_idx = 0; chunk_idx < count; ++chunk_idx, ++row_idx) {
			if (boundary_end.CellIsNull(chunk_idx)) {
				window_end = peer_end_data[chunk_idx];
			} else {
				//	Count peer groups forward.
				const auto peer_end = peer_end_data[chunk_idx];
				const auto partition_end = partition_end_data[chunk_idx];
				const auto boundary = boundary_end.GetCell<int64_t>(chunk_idx);
				if (boundary < 0) {
					throw OutOfRangeException("Invalid GROUPS FOLLOWING value");
				} else if (!boundary) {
					window_end = peer_end;
				} else {
					auto n = UnsafeNumericCast<idx_t>(boundary);
					window_end = FindNextStart(order_mask, peer_end + 1, partition_end, n);
				}
			}
			frame_end_data[chunk_idx] = window_end;
		}
		break;
	case WindowBoundary::UNBOUNDED_PRECEDING:
	case WindowBoundary::INVALID:
		throw InternalException("Unsupported window end boundary");
	}

	ClampFrame(count, frame_end_data, partition_begin_data, partition_end_data);
}

} // namespace duckdb
