#pragma once

#include <statdaemons/Increment.h>

#include <DB/Core/SortDescription.h>
#include <DB/Interpreters/Context.h>
#include <DB/Interpreters/ExpressionActions.h>
#include <DB/Storages/IStorage.h>
#include <DB/Storages/MergeTree/ActiveDataPartSet.h>
#include <DB/IO/ReadBufferFromString.h>
#include <DB/IO/WriteBufferFromFile.h>
#include <DB/Common/escapeForFileName.h>
#include <Poco/RWLock.h>


#define MERGE_TREE_MARK_SIZE (2 * sizeof(size_t))


namespace DB
{

/** Структура данных для *MergeTree движков.
  * Используется merge tree для инкрементальной сортировки данных.
  * Таблица представлена набором сортированных кусков.
  * При вставке, данные сортируются по указанному выражению (первичному ключу) и пишутся в новый кусок.
  * Куски объединяются в фоне, согласно некоторой эвристике.
  * Для каждого куска, создаётся индексный файл, содержащий значение первичного ключа для каждой n-ой строки.
  * Таким образом, реализуется эффективная выборка по диапазону первичного ключа.
  *
  * Дополнительно:
  *
  *  Указывается столбец, содержащий дату.
  *  Для каждого куска пишется минимальная и максимальная дата.
  *  (по сути - ещё один индекс)
  *
  *  Данные разделяются по разным месяцам (пишутся в разные куски для разных месяцев).
  *  Куски для разных месяцев не объединяются - для простоты эксплуатации.
  *  (дают локальность обновлений, что удобно для синхронизации и бэкапа)
  *
  * Структура файлов:
  *  / min-date _ max-date _ min-id _ max-id _ level / - директория с куском.
  * Внутри директории с куском:
  *  checksums.txt - список файлов с их размерами и контрольными суммами.
  *  columns.txt - список столбцов с их типами.
  *  primary.idx - индексный файл.
  *  Column.bin - данные столбца
  *  Column.mrk - засечки, указывающие, откуда начинать чтение, чтобы пропустить n * k строк.
  *
  * Имеется несколько режимов работы, определяющих, что делать при мердже:
  * - Ordinary - ничего дополнительно не делать;
  * - Collapsing - при склейке кусков "схлопывать"
  *   пары записей с разными значениями sign_column для одного значения первичного ключа.
  *   (см. CollapsingSortedBlockInputStream.h)
  * - Summing - при склейке кусков, при совпадении PK суммировать все числовые столбцы, не входящие в PK.
  * - Aggregating - при склейке кусков, при совпадении PK, делается слияние состояний столбцов-агрегатных функций.
  */

/** Этот класс хранит список кусков и параметры структуры данных.
  * Для чтения и изменения данных используются отдельные классы:
  *  - MergeTreeDataSelectExecutor
  *  - MergeTreeDataWriter
  *  - MergeTreeDataMerger
  */

struct MergeTreeSettings
{
	/// Опеределяет, насколько разбалансированные объединения мы готовы делать.
	/// Чем больше, тем более разбалансированные. Желательно, чтобы было больше, чем 1/max_parts_to_merge_at_once.
	double size_ratio_coefficient_to_merge_parts = 0.25;

	/// Сколько за раз сливать кусков.
	/// Трудоемкость выбора кусков O(N * max_parts_to_merge_at_once).
	size_t max_parts_to_merge_at_once = 10;

	/// Куски настолько большого размера объединять нельзя вообще.
	size_t max_bytes_to_merge_parts = 25ul * 1024 * 1024 * 1024;

	/// Не больше половины потоков одновременно могут выполнять слияния, в которых участвует хоть один кусок хотя бы такого размера.
	size_t max_bytes_to_merge_parts_small = 250 * 1024 * 1024;

	/// Во столько раз ночью увеличиваем коэффициент.
	size_t merge_parts_at_night_inc = 10;

	/// Сколько заданий на слияние кусков разрешено одновременно иметь в очереди ReplicatedMergeTree.
	size_t max_replicated_merges_in_queue = 6;

	/// Если из одного файла читается хотя бы столько строк, чтение можно распараллелить.
	size_t min_rows_for_concurrent_read = 20 * 8192;

	/// Можно пропускать чтение более чем стольки строк ценой одного seek по файлу.
	size_t min_rows_for_seek = 5 * 8192;

	/// Если отрезок индекса может содержать нужные ключи, делим его на столько частей и рекурсивно проверяем их.
	size_t coarse_index_granularity = 8;

	/// Максимальное количество строк на запрос, для использования кэша разжатых данных. Если запрос большой - кэш не используется.
	/// (Чтобы большие запросы не вымывали кэш.)
	size_t max_rows_to_use_cache = 1024 * 1024;

	/// Через сколько секунд удалять ненужные куски.
	time_t old_parts_lifetime = 5 * 60;

	/// Если в таблице хотя бы столько активных кусков, искусственно замедлять вставки в таблицу.
	size_t parts_to_delay_insert = 150;

	/// Если в таблице parts_to_delay_insert + k кусков, спать insert_delay_step^k миллисекунд перед вставкой каждого блока.
	/// Таким образом, скорость вставок автоматически замедлится примерно до скорости слияний.
	double insert_delay_step = 1.1;

	/// Для скольки последних блоков хранить хеши в ZooKeeper.
	size_t replicated_deduplication_window = 10000;

	/// Хранить примерно столько последних записей в логе в ZooKeeper, даже если они никому уже не нужны.
	/// Не влияет на работу таблиц; используется только чтобы успеть посмотреть на лог в ZooKeeper глазами прежде, чем его очистят.
	size_t replicated_logs_to_keep = 100;
};

class MergeTreeData : public ITableDeclaration
{
public:
	/// Функция, которую можно вызвать, если есть подозрение, что данные куска испорчены.
	typedef std::function<void (const String &)> BrokenPartCallback;

	/// Описание куска с данными.
	struct DataPart : public ActiveDataPartSet::Part
	{
		/** Контрольные суммы всех не временных файлов.
		  * Для сжатых файлов хранятся чексумма и размер разжатых данных, чтобы не зависеть от способа сжатия.
		  */
		struct Checksums
		{
			struct Checksum
			{
				size_t file_size;
				uint128 file_hash;

				bool is_compressed = false;
				size_t uncompressed_size;
				uint128 uncompressed_hash;

				Checksum() {}
				Checksum(size_t file_size_, uint128 file_hash_) : file_size(file_size_), file_hash(file_hash_) {}
				Checksum(size_t file_size_, uint128 file_hash_, size_t uncompressed_size_, uint128 uncompressed_hash_)
					: file_size(file_size_), file_hash(file_hash_), is_compressed(true),
					uncompressed_size(uncompressed_size_), uncompressed_hash(uncompressed_hash_) {}

				void checkEqual(const Checksum & rhs, bool have_uncompressed, const String & name) const;
				void checkSize(const String & path) const;
			};

			typedef std::map<String, Checksum> FileChecksums;
			FileChecksums files;

			void addFile(const String & file_name, size_t file_size, uint128 file_hash)
			{
				files[file_name] = Checksum(file_size, file_hash);
			}

			/// Проверяет, что множество столбцов и их контрольные суммы совпадают. Если нет - бросает исключение.
			/// Если have_uncompressed, для сжатых файлов сравнивает чексуммы разжатых данных. Иначе сравнивает только чексуммы файлов.
			void checkEqual(const Checksums & rhs, bool have_uncompressed) const;

			/// Проверяет, что в директории есть все нужные файлы правильных размеров. Не проверяет чексуммы.
			void checkSizes(const String & path) const;

			/// Сериализует и десериализует в человекочитаемом виде.
			bool readText(ReadBuffer & in); /// Возвращает false, если чексуммы в слишком старом формате.
			void writeText(WriteBuffer & out) const;

			bool empty() const
			{
				return files.empty();
			}

			/// Контрольная сумма от множества контрольных сумм .bin файлов.
			String summaryDataChecksum() const
			{
				SipHash hash;

				/// Пользуемся тем, что итерирование в детерминированном (лексикографическом) порядке.
				for (const auto & it : files)
				{
					const String & name = it.first;
					const Checksum & sum = it.second;
					if (name.size() < strlen(".bin") || name.substr(name.size() - 4) != ".bin")
						continue;
					size_t len = name.size();
					hash.update(reinterpret_cast<const char *>(&len), sizeof(len));
					hash.update(name.data(), len);
					hash.update(reinterpret_cast<const char *>(&sum.uncompressed_size), sizeof(sum.uncompressed_size));
					hash.update(reinterpret_cast<const char *>(&sum.uncompressed_hash), sizeof(sum.uncompressed_hash));
				}

				UInt64 lo, hi;
				hash.get128(lo, hi);
				return DB::toString(lo) + "_" + DB::toString(hi);
			}

			String toString() const
			{
				String s;
				{
					WriteBufferFromString out(s);
					writeText(out);
				}
				return s;
			}

			static Checksums parse(const String & s)
			{
				ReadBufferFromString in(s);
				Checksums res;
				if (!res.readText(in))
					throw Exception("Checksums format is too old", ErrorCodes::FORMAT_VERSION_TOO_OLD);
				assertEOF(in);
				return res;
			}
		};

 		DataPart(MergeTreeData & storage_) : storage(storage_), size(0), size_in_bytes(0), remove_time(0) {}

 		MergeTreeData & storage;

		size_t size;	/// в количестве засечек.
		volatile size_t size_in_bytes; /// размер в байтах, 0 - если не посчитано;
		                               /// используется из нескольких потоков без блокировок (изменяется при ALTER).
		time_t modification_time;
		mutable time_t remove_time = std::numeric_limits<time_t>::max(); /// Когда кусок убрали из рабочего набора.

		/// Если true, деструктор удалит директорию с куском.
		bool is_temp = false;

		/// Первичный ключ. Всегда загружается в оперативку.
		typedef std::vector<Field> Index;
		Index index;

		/// NOTE Засечки кэшируются в оперативке. См. MarkCache.h.

		Checksums checksums;

		/// Описание столбцов.
		NamesAndTypesList columns;

		/** Блокируется на запись при изменении columns, checksums или любых файлов куска.
		  * Блокируется на чтение при    чтении columns, checksums или любых файлов куска.
		  */
		mutable Poco::RWLock columns_lock;

		/** Берется на все время ALTER куска: от начала записи временных фалов до их переименования в постоянные.
		  * Берется при разлоченном columns_lock.
		  *
		  * NOTE: "Можно" было бы обойтись без этого мьютекса, если бы можно было превращать ReadRWLock в WriteRWLock, не снимая блокировку.
		  * Такое превращение невозможно, потому что создало бы дедлок, если делать его из двух потоков сразу.
		  * Взятие этого мьютекса означает, что мы хотим заблокировать columns_lock на чтение с намерением потом, не
		  *  снимая блокировку, заблокировать его на запись.
		  */
		mutable Poco::FastMutex alter_mutex;

		~DataPart()
		{
			if (is_temp)
			{
				try
				{
					Poco::File dir(storage.full_path + name);
					if (!dir.exists())
						return;

					if (name.substr(0, strlen("tmp")) != "tmp")
					{
						LOG_ERROR(storage.log, "~DataPart() should remove part " << storage.full_path + name
							<< " but its name doesn't start with tmp. Too suspicious, keeping the part.");
						return;
					}

					dir.remove(true);
				}
				catch (...)
				{
					tryLogCurrentException(__PRETTY_FUNCTION__);
				}
			}
		}

		/// Вычисляем сумарный размер всей директории со всеми файлами
		static size_t calcTotalSize(const String &from)
		{
			Poco::File cur(from);
			if (cur.isFile())
				return cur.getSize();
			std::vector<std::string> files;
			cur.list(files);
			size_t res = 0;
			for (size_t i = 0; i < files.size(); ++i)
				res += calcTotalSize(from + files[i]);
			return res;
		}

		void remove() const
		{
			String from = storage.full_path + name + "/";
			String to = storage.full_path + "tmp2_" + name + "/";

			Poco::File(from).renameTo(to);
			Poco::File(to).remove(true);
		}

		/// Переименовывает кусок, дописав к имени префикс.
		void renameAddPrefix(const String & prefix) const
		{
			String from = storage.full_path + name + "/";
			String to = storage.full_path + prefix + name + "/";

			Poco::File f(from);
			f.setLastModified(Poco::Timestamp::fromEpochTime(time(0)));
			f.renameTo(to);
		}

		/// Загрузить индекс и вычислить размер. Если size=0, вычислить его тоже.
		void loadIndex()
		{
			/// Размер - в количестве засечек.
			if (!size)
				size = Poco::File(storage.full_path + name + "/" + escapeForFileName(columns.front().name) + ".mrk")
					.getSize() / MERGE_TREE_MARK_SIZE;

			size_t key_size = storage.sort_descr.size();
			index.resize(key_size * size);

			String index_path = storage.full_path + name + "/primary.idx";
			ReadBufferFromFile index_file(index_path,
				std::min(static_cast<size_t>(DBMS_DEFAULT_BUFFER_SIZE), Poco::File(index_path).getSize()));

			for (size_t i = 0; i < size; ++i)
				for (size_t j = 0; j < key_size; ++j)
					storage.primary_key_sample.getByPosition(j).type->deserializeBinary(index[i * key_size + j], index_file);

			if (!index_file.eof())
				throw Exception("index file " + index_path + " is unexpectedly long", ErrorCodes::EXPECTED_END_OF_FILE);

			size_in_bytes = calcTotalSize(storage.full_path + name + "/");
		}

		/// Прочитать контрольные суммы, если есть.
		void loadChecksums()
		{
			String path = storage.full_path + name + "/checksums.txt";
			if (!Poco::File(path).exists())
			{
				if (storage.require_part_metadata)
					throw Exception("No checksums.txt in part " + name, ErrorCodes::NO_FILE_IN_DATA_PART);

				return;
			}
			ReadBufferFromFile file(path, std::min(static_cast<size_t>(DBMS_DEFAULT_BUFFER_SIZE), Poco::File(path).getSize()));
			if (checksums.readText(file))
				assertEOF(file);
		}

		void loadColumns()
		{
			String path = storage.full_path + name + "/columns.txt";
			if (!Poco::File(path).exists())
			{
				if (storage.require_part_metadata)
					throw Exception("No columns.txt in part " + name, ErrorCodes::NO_FILE_IN_DATA_PART);
				columns = *storage.columns;

				/// Если нет файла со списком столбцов, запишем его.
				{
					WriteBufferFromFile out(path + ".tmp", 4096);
					columns.writeText(out);
				}
				Poco::File(path + ".tmp").renameTo(path);

				return;
			}

			ReadBufferFromFile file(path, std::min(static_cast<size_t>(DBMS_DEFAULT_BUFFER_SIZE), Poco::File(path).getSize()));
			columns.readText(file, storage.context.getDataTypeFactory());
		}

		void checkNotBroken()
		{
			String path = storage.full_path + name;

			if (!checksums.empty())
			{
				if (!checksums.files.count("primary.idx"))
					throw Exception("No checksum for primary.idx", ErrorCodes::NO_FILE_IN_DATA_PART);

				if (storage.require_part_metadata)
				{
					for (const NameAndTypePair & it : columns)
					{
						String name = escapeForFileName(it.name);
						if (!checksums.files.count(name + ".mrk") ||
							!checksums.files.count(name + ".bin"))
							throw Exception("No .mrk or .bin file checksum for column " + name, ErrorCodes::NO_FILE_IN_DATA_PART);
					}
				}

				checksums.checkSizes(path + "/");
			}
			else
			{
				/// Проверяем, что первичный ключ непуст.

				Poco::File index_file(path + "/primary.idx");

				if (!index_file.exists() || index_file.getSize() == 0)
					throw Exception("Part " + path + " is broken: primary key is empty.", ErrorCodes::BAD_SIZE_OF_FILE_IN_DATA_PART);

				/// Проверяем, что все засечки непусты и имеют одинаковый размер.

				ssize_t marks_size = -1;
				for (const NameAndTypePair & it : columns)
				{
					Poco::File marks_file(path + "/" + escapeForFileName(it.name) + ".mrk");

					/// При добавлении нового столбца в таблицу файлы .mrk не создаются. Не будем ничего удалять.
					if (!marks_file.exists())
						continue;

					if (marks_size == -1)
					{
						marks_size = marks_file.getSize();

						if (0 == marks_size)
							throw Exception("Part " + path + " is broken: " + marks_file.path() + " is empty.",
								ErrorCodes::BAD_SIZE_OF_FILE_IN_DATA_PART);
					}
					else
					{
						if (static_cast<ssize_t>(marks_file.getSize()) != marks_size)
							throw Exception("Part " + path + " is broken: marks have different sizes.",
								ErrorCodes::BAD_SIZE_OF_FILE_IN_DATA_PART);
					}
				}
			}
		}

		bool hasColumnFiles(const String & column) const
		{
			String escaped_column = escapeForFileName(column);
			return Poco::File(storage.full_path + name + "/" + escaped_column + ".bin").exists() &&
			       Poco::File(storage.full_path + name + "/" + escaped_column + ".mrk").exists();
		}
	};

	typedef std::shared_ptr<DataPart> MutableDataPartPtr;
	/// После добавление в рабочее множество DataPart нельзя изменять.
	typedef std::shared_ptr<const DataPart> DataPartPtr;
	struct DataPartPtrLess { bool operator() (const DataPartPtr & lhs, const DataPartPtr & rhs) const { return *lhs < *rhs; } };
	typedef std::set<DataPartPtr, DataPartPtrLess> DataParts;
	typedef std::vector<DataPartPtr> DataPartsVector;


	/// Некоторые операции над множеством кусков могут возвращать такой объект.
	/// Если не был вызван commit, деструктор откатывает операцию.
	class Transaction : private boost::noncopyable
	{
	public:
		Transaction() {}

		void commit()
		{
			data = nullptr;
			removed_parts.clear();
			added_parts.clear();
		}

		~Transaction()
		{
			try
			{
				if (data && (!removed_parts.empty() || !added_parts.empty()))
				{
					LOG_DEBUG(data->log, "Undoing transaction");
					data->replaceParts(removed_parts, added_parts, true);
				}
			}
			catch(...)
			{
				tryLogCurrentException("~MergeTreeData::Transaction");
			}
		}
	private:
		friend class MergeTreeData;

		MergeTreeData * data = nullptr;
		DataPartsVector removed_parts;
		DataPartsVector added_parts;
	};

	/// Объект, помнящий какие временные файлы были созданы в директории с куском в ходе изменения (ALTER) его столбцов.
	class AlterDataPartTransaction : private boost::noncopyable
	{
	public:
		/// Переименовывает временные файлы, завершая ALTER куска.
		void commit();

		/// Если не был вызван commit(), удаляет временные файлы, отменяя ALTER куска.
		~AlterDataPartTransaction();

	private:
		friend class MergeTreeData;

		AlterDataPartTransaction(DataPartPtr data_part_) : data_part(data_part_), alter_lock(data_part->alter_mutex) {}

		void clear()
		{
			alter_lock.unlock();
			data_part = nullptr;
		}

		DataPartPtr data_part;
		Poco::ScopedLockWithUnlock<Poco::FastMutex> alter_lock;

		DataPart::Checksums new_checksums;
		NamesAndTypesList new_columns;
		/// Если значение - пустая строка, файл нужно удалить, и он не временный.
		NameToNameMap rename_map;
	};

	typedef std::unique_ptr<AlterDataPartTransaction> AlterDataPartTransactionPtr;

	/// Режим работы. См. выше.
	enum Mode
	{
		Ordinary,
		Collapsing,
		Summing,
		Aggregating,
	};

	static void doNothing(const String & name) {}

	/** Подцепить таблицу с соответствующим именем, по соответствующему пути (с / на конце),
	  *  (корректность имён и путей не проверяется)
	  *  состоящую из указанных столбцов.
	  *
	  * primary_expr_ast	- выражение для сортировки;
	  * date_column_name 	- имя столбца с датой;
	  * index_granularity 	- на сколько строчек пишется одно значение индекса.
	  * require_part_metadata - обязательно ли в директории с куском должны быть checksums.txt и columns.txt
	  */
	MergeTreeData(	const String & full_path_, NamesAndTypesListPtr columns_,
					const Context & context_,
					ASTPtr & primary_expr_ast_,
					const String & date_column_name_,
					const ASTPtr & sampling_expression_, /// nullptr, если семплирование не поддерживается.
					size_t index_granularity_,
					Mode mode_,
					const String & sign_column_,
					const MergeTreeSettings & settings_,
					const String & log_name_,
					bool require_part_metadata_,
					BrokenPartCallback broken_part_callback_ = &MergeTreeData::doNothing);

	std::string getModePrefix() const;

	bool supportsSampling() const { return !!sampling_expression; }
	bool supportsFinal() const { return !sign_column.empty(); }
	bool supportsPrewhere() const { return true; }

	UInt64 getMaxDataPartIndex();

	std::string getTableName() const {
		throw Exception("Logical error: calling method getTableName of not a table.",	ErrorCodes::LOGICAL_ERROR);
	}

	const NamesAndTypesList & getColumnsList() const { return *columns; }

	String getFullPath() const { return full_path; }

	String getLogName() const { return log_name; }

	/** Возвращает копию списка, чтобы снаружи можно было не заботиться о блокировках.
	  */
	DataParts getDataParts();
	DataParts getAllDataParts();

	/** Максимальное количество кусков в одном месяце.
	  */
	size_t getMaxPartsCountForMonth();

	/** Если в таблице слишком много активных кусков, спит некоторое время, чтобы дать им возможность смерджиться.
	  */
	void delayInsertIfNeeded();

	/** Если !including_inactive:
	  *   Возвращает активный кусок с указанным именем или кусок, покрывающий его. Если такого нет, возвращает nullptr.
	  * Если including_inactive:
	  *   Если среди all_data_parts есть кусок с именем part_name, возвращает его. Иначе делает то же, что при !including_inactive.
	  */
	DataPartPtr getContainingPart(const String & part_name, bool including_inactive = false);

	/** Переименовывает временный кусок в постоянный и добавляет его в рабочий набор.
	  * Если increment!=nullptr, индекс куска берется из инкремента. Иначе индекс куска не меняется.
	  * Предполагается, что кусок не пересекается с существующими.
	  * Если out_transaction не nullptr, присваивает туда объект, позволяющий откатить добавление куска (но не переименование).
	  */
	void renameTempPartAndAdd(MutableDataPartPtr part, Increment * increment = nullptr, Transaction * out_transaction = nullptr);

	/** То же, что renameTempPartAndAdd, но кусок может покрывать существующие куски.
	  * Удаляет и возвращает все куски, покрытые добавляемым (в возрастающем порядке).
	  */
	DataPartsVector renameTempPartAndReplace(MutableDataPartPtr part, Increment * increment = nullptr, Transaction * out_transaction = nullptr);

	/** Убирает из рабочего набора куски remove и добавляет куски add.
	  * Если clear_without_timeout, данные будут удалены при следующем clearOldParts, игнорируя old_parts_lifetime.
	  */
	void replaceParts(const DataPartsVector & remove, const DataPartsVector & add, bool clear_without_timeout);

	/** Переименовывает кусок в prefix_кусок и убирает его из рабочего набора.
	  */
	void renameAndDetachPart(DataPartPtr part, const String & prefix);

	/** Удалить неактуальные куски. Возвращает имена удаленных кусков.
	  */
	Strings clearOldParts();

	/** После вызова dropAllData больше ничего вызывать нельзя.
	  * Удаляет директорию с данными и сбрасывает кеши разжатых блоков и засечек.
	  */
	void dropAllData();

	/** Перемещает всю директорию с данными.
	  * Сбрасывает кеши разжатых блоков и засечек.
	  * Нужно вызывать под залоченным lockStructureForAlter().
	  */
	void setPath(const String & full_path);

	/* Проверить, что такой ALTER можно выполнить:
	 *  - Есть все нужные столбцы.
	 *  - Все преобразования типов допустимы.
	 *  - Не затронуты столбцы ключа, знака и семплирования.
	 * Бросает исключение, если что-то не так.
	 */
	void checkAlter(const AlterCommands & params);

	/** Выполняет ALTER куска данных, записывает результат во временные файлы.
	  * Возвращает объект, позволяющий переименовать временные файлы в постоянные.
	  * Если измененных столбцов подозрительно много, и !skip_sanity_checks, бросает исключение.
	  * Если никаких действий над данными не требуется, возвращает nullptr.
	  */
	AlterDataPartTransactionPtr alterDataPart(DataPartPtr part, const NamesAndTypesList & new_columns, bool skip_sanity_checks = false);

	/// Нужно вызывать под залоченным lockStructureForAlter().
	void setColumnsList(const NamesAndTypesList & new_columns) { columns = new NamesAndTypesList(new_columns); }

	/// Нужно вызвать, если есть подозрение, что данные куска испорчены.
	void reportBrokenPart(const String & name)
	{
		broken_part_callback(name);
	}

	ExpressionActionsPtr getPrimaryExpression() const { return primary_expr; }
	SortDescription getSortDescription() const { return sort_descr; }

	const Context & context;
	const String date_column_name;
	const ASTPtr sampling_expression;
	const size_t index_granularity;

	/// Режим работы - какие дополнительные действия делать при мердже.
	const Mode mode;
	/// Для схлопывания записей об изменениях, если используется Collapsing режим работы.
	const String sign_column;

	const MergeTreeSettings settings;

	const ASTPtr primary_expr_ast;

private:
	bool require_part_metadata;

	ExpressionActionsPtr primary_expr;
	SortDescription sort_descr;
	Block primary_key_sample;

	String full_path;

	NamesAndTypesListPtr columns;

	BrokenPartCallback broken_part_callback;

	String log_name;
	Logger * log;

	/** Актуальное множество кусков с данными. */
	DataParts data_parts;
	Poco::FastMutex data_parts_mutex;

	/** Множество всех кусков с данными, включая уже слитые в более крупные, но ещё не удалённые. Оно обычно небольшое (десятки элементов).
	  * Ссылки на кусок есть отсюда, из списка актуальных кусков и из каждого потока чтения, который его сейчас использует.
	  * То есть, если количество ссылок равно 1 - то кусок не актуален и не используется прямо сейчас, и его можно удалить.
	  */
	DataParts all_data_parts;
	Poco::FastMutex all_data_parts_mutex;

	/// Загрузить множество кусков с данными с диска. Вызывается один раз - при создании объекта.
	void loadDataParts();

	/// Определить, не битые ли данные в директории. Проверяет индекс и засечеки, но не сами данные.
	bool isBrokenPart(const String & path);

	/** Выражение, преобразующее типы столбцов.
	  * Если преобразований типов нет, out_expression=nullptr.
	  * out_rename_map отображает файлы-столбцы на выходе выражения в новые файлы таблицы.
	  * Файлы, которые нужно удалить, в out_rename_map отображаются в пустую строку.
	  * Если !part, просто проверяет, что все нужные преобразования типов допустимы.
	  */
	void createConvertExpression(DataPartPtr part, const NamesAndTypesList & old_columns, const NamesAndTypesList & new_columns,
		ExpressionActionsPtr & out_expression, NameToNameMap & out_rename_map);
};

}
