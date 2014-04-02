#pragma once

#include <Poco/Timespan.h>
#include <Poco/Util/AbstractConfiguration.h>
#include <DB/Core/Defines.h>
#include <DB/Core/Field.h>

#include <DB/Interpreters/Limits.h>
#include <DB/Interpreters/SettingsCommon.h>


namespace DB
{

/** Настройки выполнения запроса.
  */
struct Settings
{
	/** Перечисление настроек: тип, имя, значение по-умолчанию.
	  *
	  * Это сделано несколько неудобно, чтобы не перечислять настройки во многих разных местах.
	  * Замечание: можно было бы сделать полностью динамические настройки вида map: String -> Field,
	  *  но пока рано, так как в коде они используются как статический struct.
	  */

#define APPLY_FOR_SETTINGS(M) \
	/** Минимальный размер блока, готового для сжатия */ \
	M(SettingUInt64, min_compress_block_size, DEFAULT_MIN_COMPRESS_BLOCK_SIZE) \
	/** Максимальный размер блока, пригодного для сжатия */ \
	M(SettingUInt64, max_compress_block_size, DEFAULT_MAX_COMPRESS_BLOCK_SIZE) \
	/** Максимальный размер блока для чтения */ \
	M(SettingUInt64, max_block_size, DEFAULT_BLOCK_SIZE) \
	/** Максимальное количество потоков выполнения запроса */ \
	M(SettingUInt64, max_threads, DEFAULT_MAX_THREADS) \
	/** Максимальное количество соединений при распределённой обработке одного запроса (должно быть больше, чем max_threads). */ \
	M(SettingUInt64, max_distributed_connections, DEFAULT_MAX_DISTRIBUTED_CONNECTIONS) \
	/** Какую часть запроса можно прочитать в оперативку для парсинга (оставшиеся данные для INSERT, если есть, считываются позже) */ \
	M(SettingUInt64, max_query_size, DEFAULT_MAX_QUERY_SIZE) \
	/** Выполнять разные стадии конвейера выполнения запроса параллельно. */ \
	M(SettingBool, asynchronous, false) \
	/** Интервал в микросекундах для проверки, не запрошена ли остановка выполнения запроса, и отправки прогресса. */ \
	M(SettingUInt64, interactive_delay, DEFAULT_INTERACTIVE_DELAY) \
	M(SettingSeconds, connect_timeout, DBMS_DEFAULT_CONNECT_TIMEOUT_SEC) \
	/** Если следует выбрать одну из рабочих реплик. */ \
	M(SettingMilliseconds, connect_timeout_with_failover_ms, DBMS_DEFAULT_CONNECT_TIMEOUT_WITH_FAILOVER_MS) \
	M(SettingSeconds, receive_timeout, DBMS_DEFAULT_RECEIVE_TIMEOUT_SEC) \
	M(SettingSeconds, send_timeout, DBMS_DEFAULT_SEND_TIMEOUT_SEC) \
	/** Время ожидания в очереди запросов, если количество одновременно выполняющихся запросов превышает максимальное. */ \
	M(SettingMilliseconds, queue_max_wait_ms, DEFAULT_QUERIES_QUEUE_WAIT_TIME_MS) \
	/** Блокироваться в цикле ожидания запроса в сервере на указанное количество секунд. */ \
	M(SettingUInt64, poll_interval, DBMS_DEFAULT_POLL_INTERVAL) \
	/** Максимальное количество соединений с одним удалённым сервером в пуле. */ \
	M(SettingUInt64, distributed_connections_pool_size, DBMS_DEFAULT_DISTRIBUTED_CONNECTIONS_POOL_SIZE) \
	/** Максимальное количество попыток соединения с репликами. */ \
	M(SettingUInt64, connections_with_failover_max_tries, DBMS_CONNECTION_POOL_WITH_FAILOVER_DEFAULT_MAX_TRIES) \
	/** Переписывать запросы SELECT из CollapsingMergeTree с агрегатными функциями для автоматического учета поля Sign. */ \
	M(SettingBool, sign_rewrite, false) \
	/** Считать минимумы и максимумы столбцов результата. Они могут выводиться в JSON-форматах. */ \
	M(SettingBool, extremes, false) \
	/** Использовать ли кэш разжатых блоков. */ \
	M(SettingBool, use_uncompressed_cache, true) \
	/** Использовать ли SplittingAggregator вместо обычного. Он быстрее для запросов с большим состоянием агрегации. */ \
	M(SettingBool, use_splitting_aggregator, false) \
	/** Следует ли отменять выполняющийся запрос с таким же id, как новый. */ \
	M(SettingBool, replace_running_query, false) \
	\
	M(SettingLoadBalancing, load_balancing, LoadBalancing::RANDOM) \
	\
	M(SettingTotalsMode, totals_mode, TotalsMode::BEFORE_HAVING) \
	M(SettingFloat, totals_auto_threshold, 0.5) \
	\
	/** Сэмплирование по умолчанию. Если равно 1, то отключено. */ \
	M(SettingFloat, default_sample, 1.0) \

	/// Всевозможные ограничения на выполнение запроса.
	Limits limits;

#define DECLARE(TYPE, NAME, DEFAULT) \
	TYPE NAME {DEFAULT};

	APPLY_FOR_SETTINGS(DECLARE)

#undef DECLARE

	/// Установить настройку по имени.
	void set(const String & name, const Field & value);

	/// Установить настройку по имени. Прочитать сериализованное в бинарном виде значение из буфера (для межсерверного взаимодействия).
	void set(const String & name, ReadBuffer & buf);

	/** Установить настройку по имени. Прочитать значение в текстовом виде из строки (например, из конфига, или из параметра URL).
	  */
	void set(const String & name, const String & value);

	/** Установить настройки из профиля (в конфиге сервера, в одном профиле может быть перечислено много настроек).
	  * Профиль также может быть установлен с помощью функций set, как настройка profile.
	  */
	void setProfile(const String & profile_name, Poco::Util::AbstractConfiguration & config);

	/// Прочитать настройки из буфера. Они записаны как набор name-value пар, идущих подряд, заканчивающихся пустым name.
	void deserialize(ReadBuffer & buf);

	/// Записать изменённые настройки в буфер. (Например, для отправки на удалённый сервер.)
	void serialize(WriteBuffer & buf) const;
};


}
