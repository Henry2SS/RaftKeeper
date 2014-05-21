#pragma once

#include <DB/Storages/MergeTree/MergeTreeData.h>

namespace DB
{

/** Умеет выбирать куски для слияния и сливать их.
  */
class MergeTreeDataMerger
{
public:
	static const size_t NO_LIMIT = std::numeric_limits<size_t>::max();

	MergeTreeDataMerger(MergeTreeData & data_) : data(data_), log(&Logger::get(data.getLogName() + " (Merger)")), canceled(false) {}

	typedef std::function<bool (const MergeTreeData::DataPartPtr &, const MergeTreeData::DataPartPtr &)> AllowedMergingPredicate;

	/** Выбирает, какие куски слить. Использует кучу эвристик.
	  * Если merge_anything_for_old_months, для кусков за прошедшие месяцы снимается ограничение на соотношение размеров.
	  * Выбирает куски так, чтобы available_disk_space, скорее всего, хватило с запасом для их слияния.
	  *
	  * can_merge - функция, определяющая, можно ли объединить пару соседних кусков.
	  *  Эта функция должна координировать слияния со вставками и другими слияниями, обеспечивая, что:
	  *  - Куски, между которыми еще может появиться новый кусок, нельзя сливать. См. METR-7001.
	  *  - Кусок, который уже сливается с кем-то в одном месте, нельзя начать сливать в кем-то другим в другом месте.
	  */
	bool selectPartsToMerge(
		MergeTreeData::DataPartsVector & what,
		String & merged_name,
		size_t available_disk_space,
		bool merge_anything_for_old_months,
		bool aggressive,
		bool only_small,
		const AllowedMergingPredicate & can_merge);

	/// Сливает куски.
	MergeTreeData::DataPartPtr mergeParts(const MergeTreeData::DataPartsVector & parts, const String & merged_name);

	/// Примерное количество места на диске, нужное для мерджа. С запасом.
	size_t estimateDiskSpaceForMerge(const MergeTreeData::DataPartsVector & parts);

	/** Отменяет все мерджи. Все выполняющиеся сейчас вызовы mergeParts скоро бросят исключение.
	  * Все новые вызовы будут бросать исключения, пока не будет вызван uncancelAll().
	  */
	void cancelAll() { canceled = true; }

	void uncancelAll() { canceled = false; }

private:
	MergeTreeData & data;

	Logger * log;

	/// Когда в последний раз писали в лог, что место на диске кончилось (чтобы не писать об этом слишком часто).
	time_t disk_space_warning_time = 0;

	volatile bool canceled;
};

}
