#pragma once

#include <boost/function.hpp>

#include <statdaemons/Stopwatch.h>

#include <DB/Core/Names.h>

#include <DB/Interpreters/Limits.h>
#include <DB/Interpreters/Quota.h>
#include <DB/Interpreters/ProcessList.h>

#include <DB/DataStreams/IBlockInputStream.h>


namespace DB
{


/// Информация для профайлинга.
struct BlockStreamProfileInfo
{
	bool started = false;
	Stopwatch total_stopwatch {CLOCK_MONOTONIC_COARSE};	/// Время с учётом ожидания
	
	String stream_name;			/// Короткое имя потока, для которого собирается информация

	size_t rows = 0;
	size_t blocks = 0;
	size_t bytes = 0;

	/// Информация о вложенных потоках - для выделения чистого времени работы.
	typedef std::vector<const BlockStreamProfileInfo *> BlockStreamProfileInfos;
	BlockStreamProfileInfos nested_infos;

	String column_names;

	/// Собрать BlockStreamProfileInfo для ближайших в дереве источников с именем name. Пример; собрать все info для PartialSorting stream-ов.
	void collectInfosForStreamsWithName(const String & name, BlockStreamProfileInfos & res) const;

	/** Получить число строк, если бы не было LIMIT-а.
	  * Если нет LIMIT-а - возвращается 0.
	  * Если запрос не содержит ORDER BY, то число может быть занижено - возвращается количество строк в блоках, которые были прочитаны до LIMIT-а.
	  * Если запрос содержит ORDER BY, то возвращается точное число строк, которое было бы, если убрать LIMIT.
	  */
	size_t getRowsBeforeLimit() const;
	bool hasAppliedLimit() const;

	void update(Block & block);
	
	/// Методы для бинарной [де]сериализации
	void read(ReadBuffer & in);
	void write(WriteBuffer & out) const;
	
private:
	void calculateRowsBeforeLimit() const;
	
	/// Для этих полей сделаем accessor'ы, т.к. их необходимо предварительно вычислять.
	mutable bool applied_limit = false;					/// Применялся ли LIMIT
	mutable size_t rows_before_limit = 0;
	mutable bool calculated_rows_before_limit = false;	/// Вычислялось ли поле rows_before_limit
};


/** Смотрит за тем, как работает источник блоков.
  * Позволяет получить информацию для профайлинга:
  *  строк в секунду, блоков в секунду, мегабайт в секунду и т. п.
  * Позволяет остановить чтение данных (во вложенных источниках).
  */
class IProfilingBlockInputStream : public IBlockInputStream
{
public:
	Block read();

	/** Реализация по-умолчанию вызывает рекурсивно readSuffix() у всех детей, а затем readSuffixImpl() у себя.
	  * Если этот поток вызывает у детей read() в отдельном потоке, этот поведение обычно неверно:
	  * readSuffix() у ребенка нельзя вызывать в момент, когда read() того же ребенка выполняется в другом потоке.
	  * В таком случае нужно переопределить этот метод, чтобы readSuffix() у детей вызывался, например, после соединения потоков.
	  */
	void readSuffix();

	/// Получить информацию о скорости выполнения.
	const BlockStreamProfileInfo & getInfo() const;

	/** Получить "тотальные" значения.
	  * Реализация по-умолчанию берёт их из себя или из первого дочернего источника, в котором они есть.
	  * Переопределённый метод может провести некоторые вычисления. Например, применить выражение к totals дочернего источника.
	  * Тотальных значений может не быть - тогда возвращается пустой блок.
	  */
	virtual const Block & getTotals();
	
	/// То же самое для минимумов и максимумов.
	const Block & getExtremes() const;


	/** Установить колбэк прогресса выполнения.
	  * Колбэк пробрасывается во все дочерние источники.
	  * По-умолчанию, он вызывается для листовых источников, после каждого блока.
	  * (Но это может быть переопределено в методе progress())
	  * Функция принимает количество строк в последнем блоке, количество байт в последнем блоке.
	  * Следует иметь ввиду, что колбэк может вызываться из разных потоков.
	  */
	void setProgressCallback(ProgressCallback callback);


	/** В этом методе:
	  * - вызывается колбэк прогресса;
	  * - обновляется статус выполнения запроса в ProcessList-е;
	  * - проверяются ограничения и квоты, которые должны быть проверены не в рамках одного источника,
	  *   а над общим количеством потраченных ресурсов во всех источниках сразу (информация в ProcessList-е).
	  */
	virtual void progress(size_t rows, size_t bytes) { progressImpl(rows, bytes); }
	void progressImpl(size_t rows, size_t bytes);


	/** Установить указатель на элемент списка процессов.
	  * Пробрасывается во все дочерние источники.
	  * В него будет записываться общая информация о потраченных на запрос ресурсах.
	  * На основе этой информации будет проверяться квота, и некоторые ограничения.
	  * Также эта информация будет доступна в запросе SHOW PROCESSLIST.
	  */
	void setProcessListElement(ProcessList::Element * elem);


	/** Попросить прервать получение данных как можно скорее.
	  * По-умолчанию - просто выставляет флаг is_cancelled и просит прерваться всех детей.
	  * Эта функция может вызываться несколько раз, в том числе, одновременно из разных потоков.
	  */
	virtual void cancel();

	/** Требуется ли прервать получение данных.
	 */
	bool isCancelled()
	{
		return is_cancelled;
	}

	/** Какие ограничения (и квоты) проверяются.
	  * Если LIMITS_CURRENT - ограничения проверяются на количество данных, прочитанных только в этом stream-е.
	  * - используется для реализации ограничений на объём результата выполнения запроса.
	  * Если LIMITS_TOTAL, то ещё дополнительно делается проверка в колбэке прогресса,
	  *  по суммарным данным по всем листовым stream-ам, в том числе, с удалённых серверов.
	  * - используется для реализации ограничений на общий объём прочитанных (исходных) данных.
	  */
	enum LimitsMode
	{
		LIMITS_CURRENT,
		LIMITS_TOTAL,
	};

	/// Используется подмножество ограничений из Limits.
	struct LocalLimits
	{
		LimitsMode mode;
		
		size_t max_rows_to_read;
		size_t max_bytes_to_read;
		OverflowMode read_overflow_mode;

		Poco::Timespan max_execution_time;
		OverflowMode timeout_overflow_mode;

		/// В строчках в секунду.
		size_t min_execution_speed;
		/// Проверять, что скорость не слишком низкая, после прошествия указанного времени.
		Poco::Timespan timeout_before_checking_execution_speed;

		LocalLimits()
			: mode(LIMITS_CURRENT),
			max_rows_to_read(0), max_bytes_to_read(0), read_overflow_mode(OverflowMode::THROW),
			max_execution_time(0), timeout_overflow_mode(OverflowMode::THROW),
			min_execution_speed(0), timeout_before_checking_execution_speed(0)
		{
		}
	};

	/** Установить ограничения для проверки на каждый блок. */
	void setLimits(const LocalLimits & limits_)
	{
		limits = limits_;
	}

	/** Установить квоту. Если устанавливается квота на объём исходных данных,
	  * то следует ещё установить mode = LIMITS_TOTAL в LocalLimits с помощью setLimits.
	  */
	void setQuota(QuotaForIntervals & quota_)
	{
		quota = &quota_;
	}

	/// Включить рассчёт минимумов и максимумов по столбцам результата.
	void enableExtremes() { enabled_extremes = true; }

protected:
	BlockStreamProfileInfo info;
	volatile bool is_cancelled = false;
	ProgressCallback progress_callback;
	ProcessList::Element * process_list_elem = nullptr;

	bool enabled_extremes = false;

	/// Дополнительная информация, которая может образоваться в процессе работы.

	/// Тотальные значения при агрегации.
	Block totals;
	/// Минимумы и максимумы. Первая строчка блока - минимумы, вторая - максимумы.
	Block extremes;

	/// Ограничения и квоты.
	
	LocalLimits limits;

	QuotaForIntervals * quota = nullptr;	/// Если nullptr - квота не используется.
	double prev_elapsed = 0;

	/// Наследники должны реализовать эту функцию.
	virtual Block readImpl() = 0;

	/// Здесь необходимо делать финализацию, которая может привести к исключению.
	virtual void readSuffixImpl() {}

	void updateExtremes(Block & block);

	/** Проверить ограничения и квоты.
	  * Но только те, что могут быть проверены в рамках каждого отдельного источника.
	  */
	bool checkLimits();
	void checkQuota(Block & block);
};

}
