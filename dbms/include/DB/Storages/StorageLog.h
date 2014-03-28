#pragma once

#include <set>

#include <Poco/File.h>
#include <Poco/RWLock.h>

#include <DB/Core/NamesAndTypes.h>
#include <DB/IO/ReadBufferFromFile.h>
#include <DB/IO/WriteBufferFromFile.h>
#include <DB/IO/CompressedReadBuffer.h>
#include <DB/IO/CompressedWriteBuffer.h>
#include <DB/Storages/IStorage.h>
#include <DB/DataStreams/IProfilingBlockInputStream.h>
#include <DB/DataStreams/IBlockOutputStream.h>


namespace DB
{

class StorageLog;


/** Смещение до каждой некоторой пачки значений.
  * Эти пачки имеют одинаковый размер в разных столбцах.
  * Они нужны, чтобы можно было читать данные в несколько потоков.
  */
struct Mark
{
	size_t rows;	/// Сколько строк содержится в этой пачке и всех предыдущих.
	size_t offset;	/// Смещение до пачки в сжатом файле.
};

typedef std::vector<Mark> Marks;


class LogBlockInputStream : public IProfilingBlockInputStream
{
public:
	LogBlockInputStream(size_t block_size_, const Names & column_names_, StorageLog & storage_, size_t mark_number_, size_t rows_limit_);
	String getName() const { return "LogBlockInputStream"; }

	String getID() const;

protected:
	Block readImpl();
private:
	size_t block_size;
	Names column_names;
	StorageLog & storage;
	size_t mark_number;		/// С какой засечки читать данные
	size_t rows_limit;		/// Максимальное количество строк, которых можно прочитать

	size_t rows_read;
	size_t current_mark;

	struct Stream
	{
		Stream(const std::string & data_path, size_t offset)
			: plain(data_path, std::min(static_cast<size_t>(DBMS_DEFAULT_BUFFER_SIZE), Poco::File(data_path).getSize())),
			compressed(plain)
		{
			if (offset)
				plain.seek(offset);
		}
		
		ReadBufferFromFile plain;
		CompressedReadBuffer compressed;
	};
	
	typedef std::map<std::string, SharedPtr<Stream> > FileStreams;
	FileStreams streams;

	void addStream(const String & name, const IDataType & type, size_t level = 0);
	void readData(const String & name, const IDataType & type, IColumn & column, size_t max_rows_to_read, size_t level = 0, bool read_offsets = true);
};


class LogBlockOutputStream : public IBlockOutputStream
{
public:
	LogBlockOutputStream(StorageLog & storage_);
	void write(const Block & block);
	void writeSuffix();
private:
	StorageLog & storage;
	Poco::ScopedWriteRWLock lock;

	struct Stream
	{
		Stream(const std::string & data_path, size_t max_compress_block_size) :
			plain(data_path, max_compress_block_size, O_APPEND | O_CREAT | O_WRONLY),
			compressed(plain)
		{
			plain_offset = Poco::File(data_path).getSize();
		}
		
		WriteBufferFromFile plain;
		CompressedWriteBuffer compressed;

		size_t plain_offset;	/// Сколько байт было в файле на момент создания LogBlockOutputStream.

		void finalize()
		{
			compressed.next();
			plain.next();
		}
	};

	typedef std::vector<std::pair<size_t, Mark> > MarksForColumns;
	
	typedef std::map<std::string, SharedPtr<Stream> > FileStreams;
	FileStreams streams;
	
	typedef std::set<std::string> OffsetColumns;
	
	WriteBufferFromFile marks_stream; /// Объявлен ниже lock, чтобы файл открывался при захваченном rwlock.

	void addStream(const String & name, const IDataType & type, size_t level = 0);
	void writeData(const String & name, const IDataType & type, const IColumn & column, MarksForColumns & out_marks, OffsetColumns & offset_columns, size_t level = 0);
	void writeMarks(MarksForColumns marks);
};


/** Реализует хранилище, подходящее для логов.
  * Ключи не поддерживаются.
  * Данные хранятся в сжатом виде.
  */
class StorageLog : public IStorage
{
friend class LogBlockInputStream;
friend class LogBlockOutputStream;

public:
	/** Подцепить таблицу с соответствующим именем, по соответствующему пути (с / на конце),
	  *  (корректность имён и путей не проверяется)
	  *  состоящую из указанных столбцов; создать файлы, если их нет.
	  */
	static StoragePtr create(const std::string & path_, const std::string & name_, NamesAndTypesListPtr columns_, size_t max_compress_block_size_ = DEFAULT_MAX_COMPRESS_BLOCK_SIZE);
	
	std::string getName() const { return "Log"; }
	std::string getTableName() const { return name; }

	const NamesAndTypesList & getColumnsList() const { return *columns; }

	virtual BlockInputStreams read(
		const Names & column_names,
		ASTPtr query,
		const Settings & settings,
		QueryProcessingStage::Enum & processed_stage,
		size_t max_block_size = DEFAULT_BLOCK_SIZE,
		unsigned threads = 1);

	BlockOutputStreamPtr write(
		ASTPtr query);

	void rename(const String & new_path_to_db, const String & new_name);

protected:
	String path;
	String name;
	NamesAndTypesListPtr columns;

	Poco::RWLock rwlock;

	/// Название виртуального столбца, отвечающего за имя таблицы, из которой идет чтение. (Например "_table")
	/// По умолчанию виртуальный столбец не поддерживается, но, например, он поддерживается в StorageChunks
	String _table_column_name;

	/// По номеру засечки получить имя таблицы, из которой идет чтение и номер последней засечки из этой таблицы.
	/// По умолчанию виртуальный столбец не поддерживается, а значит при попытке его чтения нужно выбросить исключение.
	virtual std::pair<String, size_t> getTableFromMark(size_t mark) const
	{
		throw Exception("There is no column " + _table_column_name + " in table " + getTableName(), ErrorCodes::NO_SUCH_COLUMN_IN_TABLE);
	}

	StorageLog(const std::string & path_, const std::string & name_, NamesAndTypesListPtr columns_, size_t max_compress_block_size_);
	
	/// Прочитать файлы с засечками, если они ещё не прочитаны.
	/// Делается лениво, чтобы при большом количестве таблиц, сервер быстро стартовал.
	/// Нельзя вызывать с залоченным на запись rwlock.
	void loadMarks();
	
	/// Можно вызывать при любом состоянии rwlock.
	size_t marksCount();
	
	BlockInputStreams read(
		size_t from_mark,
		size_t to_mark,
		const Names & column_names,
		ASTPtr query,
		const Settings & settings,
		QueryProcessingStage::Enum & processed_stage,
		size_t max_block_size = DEFAULT_BLOCK_SIZE,
		unsigned threads = 1);
	
private:
	/// Данные столбца
	struct ColumnData
	{
		/// Задает номер столбца в файле с засечками.
		/// Не обязательно совпадает с номером столбца среди столбцов таблицы: здесь нумеруются также столбцы с длинами массивов.
		size_t column_index;
		
		Poco::File data_file;
		Marks marks;
	};
	typedef std::map<String, ColumnData> Files_t;
	Files_t files; /// name -> data
	Names column_names; /// column_index -> name
	
	Poco::File marks_file;
	
	/// Порядок добавления файлов не должен меняться: он соответствует порядку столбцов в файле с засечками.
	void addFile(const String & column_name, const IDataType & type, size_t level = 0);

	bool loaded_marks;

	size_t max_compress_block_size;

	/** Для обычных столбцов, в засечках указано количество строчек в блоке.
	  * Для столбцов-массивов и вложенных структур, есть более одной группы засечек, соответствующих разным файлам:
	  *  - для внутренностей (файла name.bin) - указано суммарное количество элементов массивов в блоке,
	  *  - для размеров массивов (файла name.size0.bin) - указано количество строчек (самих целых массивов) в блоке.
	  *
	  * Вернуть первую попавшуюся группу засечек, в которых указано количество строчек, а не внутренностей массивов.
	  */
	const Marks & getMarksWithRealRowCount() const;
};

}
