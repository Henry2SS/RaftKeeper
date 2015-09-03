#pragma once

#include <DB/Core/NamesAndTypes.h>
#include <DB/Storages/MergeTree/RangesInDataPart.h>
#include <statdaemons/ext/range.hpp>
#include <mutex>


namespace DB
{


struct MergeTreeReadTask
{
	MergeTreeData::DataPartPtr data_part;
	MarkRanges mark_ranges;
	std::size_t part_index_in_query;
	const Names & ordered_names;
	const NameSet & column_name_set;
	const NamesAndTypesList & columns;
	const NamesAndTypesList & pre_columns;
	const bool remove_prewhere_column;
	const bool should_reorder;

	MergeTreeReadTask(
		const MergeTreeData::DataPartPtr & data_part, const MarkRanges & ranges, const std::size_t part_index_in_query,
		const Names & ordered_names, const NameSet & column_name_set, const NamesAndTypesList & columns,
		const NamesAndTypesList & pre_columns, const bool remove_prewhere_column, const bool should_reorder)
		: data_part{data_part}, mark_ranges{ranges}, part_index_in_query{part_index_in_query},
		  ordered_names{ordered_names}, column_name_set{column_name_set}, columns{columns}, pre_columns{pre_columns},
		  remove_prewhere_column{remove_prewhere_column}, should_reorder{should_reorder}
	{}
};

using MergeTreeReadTaskPtr = std::unique_ptr<MergeTreeReadTask>;

class MergeTreeReadPool
{
public:
	MergeTreeReadPool(
		const std::size_t threads, const std::size_t sum_marks, const std::size_t min_marks_for_concurrent_read,
		RangesInDataParts parts, MergeTreeData & data, const ExpressionActionsPtr & prewhere_actions,
		const String & prewhere_column_name, const bool check_columns, const Names & column_names,
		const bool do_not_steal_tasks = false)
		: data{data}, column_names{column_names}, do_not_steal_tasks{do_not_steal_tasks}
	{
		const auto per_part_sum_marks = fillPerPartInfo(parts, prewhere_actions, prewhere_column_name, check_columns);
		fillPerThreadInfo(threads, sum_marks, per_part_sum_marks, parts, min_marks_for_concurrent_read);
	}

	MergeTreeReadPool(const MergeTreeReadPool &) = delete;
	MergeTreeReadPool & operator=(const MergeTreeReadPool &) = delete;

	MergeTreeReadTaskPtr getTask(const std::size_t min_marks_to_read, const std::size_t thread)
	{
		const std::lock_guard<std::mutex> lock{mutex};

		if (remaining_thread_tasks.empty())
			return nullptr;

		const auto tasks_remaining_for_this_thread = !threads_tasks[thread].sum_marks_in_parts.empty();
		if (!tasks_remaining_for_this_thread && do_not_steal_tasks)
			return nullptr;

		const auto thread_idx = tasks_remaining_for_this_thread ? thread : *std::begin(remaining_thread_tasks);
		auto & thread_tasks = threads_tasks[thread_idx];

		auto & thread_task = thread_tasks.parts_and_ranges.back();
		const auto part_idx = thread_task.part_idx;

		auto & part = parts[part_idx];
		auto & marks_in_part = thread_tasks.sum_marks_in_parts.back();

		/// Берём весь кусок, если он достаточно мал
		auto need_marks = std::min(marks_in_part, min_marks_to_read);

		/// Не будем оставлять в куске слишком мало строк.
		if (marks_in_part > need_marks &&
			marks_in_part - need_marks < min_marks_to_read)
			need_marks = marks_in_part;

		MarkRanges ranges_to_get_from_part;

		/// Возьмем весь кусок, если он достаточно мал.
		if (marks_in_part <= need_marks)
		{
			const auto marks_to_get_from_range = marks_in_part;

			/// Восстановим порядок отрезков.
			std::reverse(thread_task.ranges.begin(), thread_task.ranges.end());

			ranges_to_get_from_part = thread_task.ranges;

			marks_in_part -= marks_to_get_from_range;

			thread_tasks.parts_and_ranges.pop_back();
			thread_tasks.sum_marks_in_parts.pop_back();

			if (thread_tasks.sum_marks_in_parts.empty())
				remaining_thread_tasks.erase(thread_idx);
		}
		else
		{
			/// Цикл по отрезкам куска.
			while (need_marks > 0 && !thread_task.ranges.empty())
			{
				auto & range = thread_task.ranges.back();

				const std::size_t marks_in_range = range.end - range.begin;
				const std::size_t marks_to_get_from_range = std::min(marks_in_range, need_marks);

				ranges_to_get_from_part.emplace_back(range.begin, range.begin + marks_to_get_from_range);
				range.begin += marks_to_get_from_range;
				if (range.begin == range.end)
				{
					std::swap(range, thread_task.ranges.back());
					thread_task.ranges.pop_back();
				}

				marks_in_part -= marks_to_get_from_range;
				need_marks -= marks_to_get_from_range;
			}
		}

		return std::make_unique<MergeTreeReadTask>(
			part.data_part, ranges_to_get_from_part, part.part_index_in_query, column_names,
			per_part_column_name_set[part_idx], per_part_columns[part_idx], per_part_pre_columns[part_idx],
			per_part_remove_prewhere_column[part_idx], per_part_should_reorder[part_idx]);
	}

public:
	std::vector<std::size_t> fillPerPartInfo(
		RangesInDataParts & parts, const ExpressionActionsPtr & prewhere_actions, const String & prewhere_column_name,
		const bool check_columns)
	{
		std::vector<std::size_t> per_part_sum_marks;

		for (const auto i : ext::range(0, parts.size()))
		{
			auto & part = parts[i];

			/// Посчитаем засечки для каждого куска.
			size_t sum_marks = 0;
			/// Отрезки уже перечислены справа налево, reverse в MergeTreeDataSelectExecutor.
			for (const auto & range : part.ranges)
				sum_marks += range.end - range.begin;

			per_part_sum_marks.push_back(sum_marks);

			per_part_columns_lock.push_back(std::make_unique<Poco::ScopedReadRWLock>(
				part.data_part->columns_lock));

			/// inject column names required for DEFAULT evaluation in current part
			auto required_column_names = column_names;

			const auto injected_columns = injectRequiredColumns(part.data_part, required_column_names);
			auto should_reoder = !injected_columns.empty();

			Names required_pre_column_names;

			if (prewhere_actions)
			{
				/// collect columns required for PREWHERE evaluation
				required_pre_column_names = prewhere_actions->getRequiredColumns();

				/// there must be at least one column required for PREWHERE
				if (required_pre_column_names.empty())
					required_pre_column_names.push_back(required_column_names[0]);

				/// PREWHERE columns may require some additional columns for DEFAULT evaluation
				const auto injected_pre_columns = injectRequiredColumns(part.data_part, required_pre_column_names);
				if (!injected_pre_columns.empty())
					should_reoder = true;

				/// will be used to distinguish between PREWHERE and WHERE columns when applying filter
				const NameSet pre_name_set{
					std::begin(required_pre_column_names), std::end(required_pre_column_names)
				};
				/** Если выражение в PREWHERE - не столбец таблицы, не нужно отдавать наружу столбец с ним
				 *	(от storage ожидают получить только столбцы таблицы). */
				per_part_remove_prewhere_column.push_back(0 == pre_name_set.count(prewhere_column_name));

				Names post_column_names;
				for (const auto & name : required_column_names)
					if (!pre_name_set.count(name))
						post_column_names.push_back(name);

				required_column_names = post_column_names;
			}
			else
				per_part_remove_prewhere_column.push_back(false);

			per_part_column_name_set.emplace_back(std::begin(required_column_names), std::end(required_column_names));

			if (check_columns)
			{
				/** Под part->columns_lock проверим, что все запрошенные столбцы в куске того же типа, что в таблице.
				 *	Это может быть не так во время ALTER MODIFY. */
				if (!required_pre_column_names.empty())
					data.check(part.data_part->columns, required_pre_column_names);
				if (!required_column_names.empty())
					data.check(part.data_part->columns, required_column_names);

				per_part_pre_columns.push_back(data.getColumnsList().addTypes(required_pre_column_names));
				per_part_columns.push_back(data.getColumnsList().addTypes(required_column_names));
			}
			else
			{
				per_part_pre_columns.push_back(part.data_part->columns.addTypes(required_pre_column_names));
				per_part_columns.push_back(part.data_part->columns.addTypes(required_column_names));
			}

			per_part_should_reorder.push_back(should_reoder);

			this->parts.push_back({ part.data_part, part.part_index_in_query });
		}

		return per_part_sum_marks;
	}

	void fillPerThreadInfo(
		const std::size_t threads, const std::size_t sum_marks, std::vector<std::size_t> per_part_sum_marks,
		RangesInDataParts & parts, const std::size_t min_marks_for_concurrent_read)
	{
		threads_tasks.resize(threads);

		const size_t min_marks_per_thread = (sum_marks - 1) / threads + 1;

		for (std::size_t i = 0; i < threads && !parts.empty(); ++i)
		{
			auto need_marks = min_marks_per_thread;

			while (need_marks > 0 && !parts.empty())
			{
				const auto part_idx = parts.size() - 1;
				RangesInDataPart & part = parts.back();
				size_t & marks_in_part = per_part_sum_marks.back();

				/// Не будем брать из куска слишком мало строк.
				if (marks_in_part >= min_marks_for_concurrent_read &&
					need_marks < min_marks_for_concurrent_read)
					need_marks = min_marks_for_concurrent_read;

				/// Не будем оставлять в куске слишком мало строк.
				if (marks_in_part > need_marks &&
					marks_in_part - need_marks < min_marks_for_concurrent_read)
					need_marks = marks_in_part;

				MarkRanges ranges_to_get_from_part;
				size_t marks_in_ranges = need_marks;

				/// Возьмем весь кусок, если он достаточно мал.
				if (marks_in_part <= need_marks)
				{
					/// Оставим отрезки перечисленными справа налево для удобства.
					ranges_to_get_from_part = part.ranges;
					marks_in_ranges = marks_in_part;

					need_marks -= marks_in_part;
					parts.pop_back();
					per_part_sum_marks.pop_back();
				}
				else
				{
					/// Цикл по отрезкам куска.
					while (need_marks > 0)
					{
						if (part.ranges.empty())
							throw Exception("Unexpected end of ranges while spreading marks among threads", ErrorCodes::LOGICAL_ERROR);

						MarkRange & range = part.ranges.back();

						const size_t marks_in_range = range.end - range.begin;
						const size_t marks_to_get_from_range = std::min(marks_in_range, need_marks);

						ranges_to_get_from_part.emplace_back(range.begin, range.begin + marks_to_get_from_range);
						range.begin += marks_to_get_from_range;
						marks_in_part -= marks_to_get_from_range;
						need_marks -= marks_to_get_from_range;
						if (range.begin == range.end)
							part.ranges.pop_back();
					}

					/// Вновь перечислим отрезки справа налево, чтобы .getTask() мог забирать их с помощью .pop_back().
					std::reverse(std::begin(ranges_to_get_from_part), std::end(ranges_to_get_from_part));
				}

				threads_tasks[i].parts_and_ranges.push_back({ part_idx, ranges_to_get_from_part });
				threads_tasks[i].sum_marks_in_parts.push_back(marks_in_ranges);
				if (marks_in_ranges != 0)
					remaining_thread_tasks.insert(i);
			}
		}
	}

	/** Если некоторых запрошенных столбцов нет в куске,
	 *	то выясняем, какие столбцы может быть необходимо дополнительно прочитать,
	 *	чтобы можно было вычислить DEFAULT выражение для этих столбцов.
	 *	Добавляет их в columns. */
	NameSet injectRequiredColumns(const MergeTreeData::DataPartPtr & part, Names & columns) const
	{
		NameSet required_columns{std::begin(columns), std::end(columns)};
		NameSet injected_columns;

		auto all_column_files_missing = true;

		for (size_t i = 0; i < columns.size(); ++i)
		{
			const auto & column_name = columns[i];

			/// column has files and hence does not require evaluation
			if (part->hasColumnFiles(column_name))
			{
				all_column_files_missing = false;
				continue;
			}

			const auto default_it = data.column_defaults.find(column_name);
			/// columns has no explicit default expression
			if (default_it == std::end(data.column_defaults))
				continue;

			/// collect identifiers required for evaluation
			IdentifierNameSet identifiers;
			default_it->second.expression->collectIdentifierNames(identifiers);

			for (const auto & identifier : identifiers)
			{
				if (data.hasColumn(identifier))
				{
					/// ensure each column is added only once
					if (required_columns.count(identifier) == 0)
					{
						columns.emplace_back(identifier);
						required_columns.emplace(identifier);
						injected_columns.emplace(identifier);
					}
				}
			}
		}

		if (all_column_files_missing)
		{
			addMinimumSizeColumn(part, columns);
			/// correctly report added column
			injected_columns.insert(columns.back());
		}

		return injected_columns;
	}

	/** Добавить столбец минимального размера.
	  * Используется в случае, когда ни один столбец не нужен, но нужно хотя бы знать количество строк.
	  * Добавляет в columns.
	  */
	void addMinimumSizeColumn(const MergeTreeData::DataPartPtr & part, Names & columns) const
	{
		const auto get_column_size = [this, &part] (const String & name) {
			const auto & files = part->checksums.files;

			const auto escaped_name = escapeForFileName(name);
			const auto bin_file_name = escaped_name + ".bin";
			const auto mrk_file_name = escaped_name + ".mrk";

			return files.find(bin_file_name)->second.file_size + files.find(mrk_file_name)->second.file_size;
		};

		const auto & storage_columns = data.getColumnsList();
		const NameAndTypePair * minimum_size_column = nullptr;
		auto minimum_size = std::numeric_limits<size_t>::max();

		for (const auto & column : storage_columns)
		{
			if (!part->hasColumnFiles(column.name))
				continue;

			const auto size = get_column_size(column.name);
			if (size < minimum_size)
			{
				minimum_size = size;
				minimum_size_column = &column;
			}
		}

		if (!minimum_size_column)
			throw Exception{
				"Could not find a column of minimum size in MergeTree",
				ErrorCodes::LOGICAL_ERROR
			};

		columns.push_back(minimum_size_column->name);
	}

	std::vector<std::unique_ptr<Poco::ScopedReadRWLock>> per_part_columns_lock;
	MergeTreeData & data;
	Names column_names;
	const bool do_not_steal_tasks;
	std::vector<NameSet> per_part_column_name_set;
	std::vector<NamesAndTypesList> per_part_columns;
	std::vector<NamesAndTypesList> per_part_pre_columns;
	/// @todo actually all of these values are either true or false for the whole query, thus no vector required
	std::vector<bool> per_part_remove_prewhere_column;
	std::vector<bool> per_part_should_reorder;

	struct part_t
	{
		MergeTreeData::DataPartPtr data_part;
		std::size_t part_index_in_query;
	};

	std::vector<part_t> parts;

	struct thread_task_t
	{
		struct part_index_and_range_t
		{
			std::size_t part_idx;
			MarkRanges ranges;
		};

		std::vector<part_index_and_range_t> parts_and_ranges;
		std::vector<std::size_t> sum_marks_in_parts;
	};

	std::vector<thread_task_t> threads_tasks;

	std::unordered_set<std::size_t> remaining_thread_tasks;

	mutable std::mutex mutex;
};

using MergeTreeReadPoolPtr = std::shared_ptr<MergeTreeReadPool>;


}
