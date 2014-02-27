#pragma once

#include <DB/Interpreters/Aggregator.h>
#include <DB/DataStreams/IProfilingBlockInputStream.h>


namespace DB
{

using Poco::SharedPtr;


/** Доагрегирует поток блоков, в котором каждый блок уже агрегирован.
  * Агрегатные функции в блоках не должны быть финализированы, чтобы их состояния можно было объединить.
  */
class MergingAggregatedBlockInputStream : public IProfilingBlockInputStream
{
public:
	MergingAggregatedBlockInputStream(BlockInputStreamPtr input_, const ColumnNumbers & keys_,
		const AggregateDescriptions & aggregates_, bool overflow_row_, bool final_)
		: aggregator(new Aggregator(keys_, aggregates_, overflow_row_)), final(final_), has_been_read(false)
	{
		children.push_back(input_);
	}

	MergingAggregatedBlockInputStream(BlockInputStreamPtr input_, const Names & keys_names_,
		const AggregateDescriptions & aggregates_, bool overflow_row_, bool final_)
		: aggregator(new Aggregator(keys_names_, aggregates_, overflow_row_)), final(final_), has_been_read(false)
	{
		children.push_back(input_);
	}

	String getName() const { return "MergingAggregatedBlockInputStream"; }

	String getID() const
	{
		std::stringstream res;
		res << "MergingAggregated(" << children.back()->getID() << ", " << aggregator->getID() << ")";
		return res.str();
	}

protected:
	Block readImpl();

private:
	SharedPtr<Aggregator> aggregator;
	bool final;
	bool has_been_read;
};

}
