#pragma once

#include <DB/DataStreams/IProfilingBlockInputStream.h>
#include <DB/Common/HashTable/HashSet.h>
#include <DB/Interpreters/AggregationCommon.h>
#include <DB/Interpreters/Limits.h>


namespace DB
{

/** Из потока блоков оставляет только уникальные строки.
  * Для реализации SELECT DISTINCT ... .
  * Если указан ненулевой limit - прекращает выдавать строки после того, как накопилось limit строк
  *  - для оптимизации SELECT DISTINCT ... LIMIT ... .
  */
class DistinctBlockInputStream : public IProfilingBlockInputStream
{
public:
	/// Пустой columns_ значит все столбцы.
	DistinctBlockInputStream(BlockInputStreamPtr input_, const Limits & limits, size_t limit_, Names columns_)
		: columns_names(columns_),
		limit(limit_),
		max_rows(limits.max_rows_in_distinct),
		max_bytes(limits.max_bytes_in_distinct),
		overflow_mode(limits.distinct_overflow_mode)
	{
		children.push_back(input_);
	}

	String getName() const override { return "DistinctBlockInputStream"; }

	String getID() const override
	{
		std::stringstream res;
		res << "Distinct(" << children.back()->getID() << ")";
		return res.str();
	}

protected:
	Block readImpl() override
	{
		/// Пока не встретится блок, после фильтрации которого что-нибудь останется, или поток не закончится.
		while (1)
		{
			/// Если уже прочитали достаточно строк - то больше читать не будем.
			if (limit && set.size() >= limit)
				return Block();

			Block block = children[0]->read();

			if (!block)
				return Block();

			size_t rows = block.rows();
			size_t columns = columns_names.empty() ? block.columns() : columns_names.size();

			ConstColumnPlainPtrs column_ptrs(columns);

			for (size_t i = 0; i < columns; ++i)
			{
				if (columns_names.empty())
					column_ptrs[i] = block.getByPosition(i).column;
				else
					column_ptrs[i] = block.getByName(columns_names[i]).column;
			}

			/// Будем фильтровать блок, оставляя там только строки, которых мы ещё не видели.
			IColumn::Filter filter(rows);

			size_t old_set_size = set.size();

			for (size_t i = 0; i < rows; ++i)
			{
				/** Уникальность строк будем отслеживать с помощью множества значений SipHash128.
				  * Делается несколько допущений.
				  * 1. Допускается неточная работа в случае коллизий SipHash128.
				  * 2. Допускается неточная работа, если строковые поля содержат нулевые байты.
				  * 3. Не поддерживаются массивы.
				  *
				  * Для оптимизации, можно добавить другие методы из Set.h.
				  */

				UInt128 key;
				SipHash hash;

				for (size_t j = 0; j < columns; ++j)
				{
					StringRef data = column_ptrs[j]->getDataAtWithTerminatingZero(i);
					hash.update(data.data, data.size);
				}

				hash.get128(key.first, key.second);

				/// Если вставилось в множество - строчку оставляем, иначе - удаляем.
				filter[i] = set.insert(key).second;

				if (limit && set.size() == limit)
					break;
			}

			/// Если ни одной новой строки не было в блоке - перейдём к следующему блоку.
			if (set.size() == old_set_size)
				continue;

			if (!checkLimits())
			{
				if (overflow_mode == OverflowMode::THROW)
					throw Exception("DISTINCT-Set size limit exceeded."
						" Rows: " + toString(set.size()) +
						", limit: " + toString(max_rows) +
						". Bytes: " + toString(set.getBufferSizeInBytes()) +
						", limit: " + toString(max_bytes) + ".",
						ErrorCodes::SET_SIZE_LIMIT_EXCEEDED);

				if (overflow_mode == OverflowMode::BREAK)
					return Block();

				throw Exception("Logical error: unknown overflow mode", ErrorCodes::LOGICAL_ERROR);
			}

			size_t all_columns = block.columns();
			for (size_t i = 0; i < all_columns; ++i)
				block.getByPosition(i).column = block.getByPosition(i).column->filter(filter);

			return block;
		}
	}

private:

	bool checkLimits() const
	{
		if (max_rows && set.size() > max_rows)
			return false;
		if (max_bytes && set.getBufferSizeInBytes() > max_bytes)
			return false;
		return true;
	}

	Names columns_names;

	size_t limit;

	/// Ограничения на максимальный размер множества
	size_t max_rows;
	size_t max_bytes;
	OverflowMode overflow_mode;

	typedef HashSet<UInt128, UInt128Hash> SetHashed;
	SetHashed set;
};

}
