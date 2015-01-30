#pragma once

#include <DB/Storages/MergeTree/MergeTreeDataSelectExecutor.h>

namespace DB
{

using Segments = std::vector<MergeTreeDataSelectExecutor::RangesInDataParts>;

/** Этот класс разбивает объект типа RangesInDataParts (см. MergeTreeDataSelectExecutor)
  * на не больше, чем указанное количество сегментов.
  */
class PartsWithRangesSplitter final
{
public:
	PartsWithRangesSplitter(const MergeTreeDataSelectExecutor::RangesInDataParts & input_,
							UInt64 parallel_replica_offset,
							size_t granularity_, size_t min_segment_size_, size_t max_segments_count_);

	~PartsWithRangesSplitter() = default;
	PartsWithRangesSplitter(const PartsWithRangesSplitter &) = delete;
	PartsWithRangesSplitter & operator=(const PartsWithRangesSplitter &) = delete;

	Segments perform();

private:
	void init();
	bool emitRange();
	bool switchToNextSegment();
	bool switchToNextRange(bool add_part);
	void initRangeInfo();
	void initSegmentInfo();
	void addPart();
	bool isRangeConsumed() const { return range_begin == range_end; }
	bool isSegmentConsumed() const { return segment_begin == segment_end; }

private:
	// Входные данные.
	const MergeTreeDataSelectExecutor::RangesInDataParts & input;
	MergeTreeDataSelectExecutor::RangesInDataParts::const_iterator input_part;
	MarkRanges::const_iterator input_range;

	// Выходные данные.
	Segments output_segments;
	Segments::iterator current_output_segment;
	MergeTreeDataSelectExecutor::RangesInDataPart * current_output_part = nullptr;

	size_t total_size = 0;

	const size_t granularity;
	const size_t min_segment_size;
	const size_t max_segments_count;

	size_t segment_size = 0;

	size_t range_begin = 0;
	size_t range_end = 0;

	size_t segment_begin = 0;
	size_t segment_end = 0;

	size_t part_index_in_query = 0;
};

}
