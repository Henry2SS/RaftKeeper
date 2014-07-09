#include <map>
#include <Poco/Path.h>

#include <DB/Common/escapeForFileName.h>

#include <DB/Core/Exception.h>
#include <DB/Core/ErrorCodes.h>

#include <DB/IO/ReadHelpers.h>
#include <DB/IO/WriteHelpers.h>

#include <DB/DataTypes/DataTypeArray.h>
#include <DB/DataTypes/DataTypeNested.h>

#include <DB/Columns/ColumnArray.h>
#include <DB/Columns/ColumnNested.h>

#include <DB/Storages/StorageTinyLog.h>


#define DBMS_STORAGE_LOG_DATA_FILE_EXTENSION 	".bin"


namespace DB
{

using Poco::SharedPtr;


TinyLogBlockInputStream::TinyLogBlockInputStream(size_t block_size_, const Names & column_names_, StorageTinyLog & storage_)
	: block_size(block_size_), column_names(column_names_), storage(storage_), finished(false)
{
}


String TinyLogBlockInputStream::getID() const
{
	std::stringstream res;
	res << "TinyLog(" << storage.getTableName() << ", " << &storage;

	for (size_t i = 0; i < column_names.size(); ++i)
		res << ", " << column_names[i];

	res << ")";
	return res.str();
}


Block TinyLogBlockInputStream::readImpl()
{
	Block res;

	if (finished || (!streams.empty() && streams.begin()->second->compressed.eof()))
	{
		/** Закрываем файлы (ещё до уничтожения объекта).
		  * Чтобы при создании многих источников, но одновременном чтении только из нескольких,
		  *  буферы не висели в памяти.
		  */
		finished = true;
		streams.clear();
		return res;
	}

	/// Если файлы не открыты, то открываем их.
	if (streams.empty())
		for (Names::const_iterator it = column_names.begin(); it != column_names.end(); ++it)
			addStream(*it, *storage.getDataTypeByName(*it));

	/// Указатели на столбцы смещений, общие для столбцов из вложенных структур данных
	typedef std::map<std::string, ColumnPtr> OffsetColumns;
	OffsetColumns offset_columns;

	for (Names::const_iterator it = column_names.begin(); it != column_names.end(); ++it)
	{
		ColumnWithNameAndType column;
		column.name = *it;
		column.type = storage.getDataTypeByName(*it);

		bool read_offsets = true;

		/// Для вложенных структур запоминаем указатели на столбцы со смещениями
		if (const DataTypeArray * type_arr = typeid_cast<const DataTypeArray *>(&*column.type))
		{
			String name = DataTypeNested::extractNestedTableName(column.name);

			if (offset_columns.count(name) == 0)
				offset_columns[name] = new ColumnArray::ColumnOffsets_t;
			else
				read_offsets = false; /// на предыдущих итерациях смещения уже считали вызовом readData

			column.column = new ColumnArray(type_arr->getNestedType()->createColumn(), offset_columns[name]);
		}
		else
			column.column = column.type->createColumn();

		readData(*it, *column.type, *column.column, block_size, 0, read_offsets);

		if (column.column->size())
			res.insert(column);
	}

	if (!res || streams.begin()->second->compressed.eof())
	{
		finished = true;
		streams.clear();
	}

	return res;
}


void TinyLogBlockInputStream::addStream(const String & name, const IDataType & type, size_t level)
{
	/// Для массивов используются отдельные потоки для размеров.
	if (const DataTypeArray * type_arr = typeid_cast<const DataTypeArray *>(&type))
	{
		String size_name = DataTypeNested::extractNestedTableName(name) + ARRAY_SIZES_COLUMN_NAME_SUFFIX + toString(level);
		if (!streams.count(size_name))
			streams.emplace(size_name, std::unique_ptr<Stream>(new Stream(storage.files[size_name].data_file.path())));

		addStream(name, *type_arr->getNestedType(), level + 1);
	}
	else if (const DataTypeNested * type_nested = typeid_cast<const DataTypeNested *>(&type))
	{
		String size_name = name + ARRAY_SIZES_COLUMN_NAME_SUFFIX + toString(level);
		streams[size_name].reset(new Stream(storage.files[size_name].data_file.path()));

		const NamesAndTypesList & columns = *type_nested->getNestedTypesList();
		for (NamesAndTypesList::const_iterator it = columns.begin(); it != columns.end(); ++it)
			addStream(DataTypeNested::concatenateNestedName(name, it->name), *it->type, level + 1);
	}
	else
		streams[name].reset(new Stream(storage.files[name].data_file.path()));
}


void TinyLogBlockInputStream::readData(const String & name, const IDataType & type, IColumn & column, size_t limit, size_t level, bool read_offsets)
{
	/// Для массивов требуется сначала десериализовать размеры, а потом значения.
	if (const DataTypeArray * type_arr = typeid_cast<const DataTypeArray *>(&type))
	{
		if (read_offsets)
		{
			type_arr->deserializeOffsets(
				column,
				streams[DataTypeNested::extractNestedTableName(name) + ARRAY_SIZES_COLUMN_NAME_SUFFIX + toString(level)]->compressed,
				limit);
		}

		if (column.size())
		{
			IColumn & nested_column = typeid_cast<ColumnArray &>(column).getData();
			size_t nested_limit = typeid_cast<ColumnArray &>(column).getOffsets()[column.size() - 1];
			readData(name, *type_arr->getNestedType(), nested_column, nested_limit, level + 1);

			if (nested_column.size() != nested_limit)
				throw Exception("Cannot read array data for all offsets", ErrorCodes::CANNOT_READ_ALL_DATA);
		}
	}
	else if (const DataTypeNested * type_nested = typeid_cast<const DataTypeNested *>(&type))
	{
		type_nested->deserializeOffsets(
			column,
			streams[name + ARRAY_SIZES_COLUMN_NAME_SUFFIX + toString(level)]->compressed,
			limit);

		if (column.size())
		{
			ColumnNested & column_nested = typeid_cast<ColumnNested &>(column);

			NamesAndTypesList::const_iterator it = type_nested->getNestedTypesList()->begin();
			for (size_t i = 0; i < column_nested.getData().size(); ++i, ++it)
			{
				readData(
					DataTypeNested::concatenateNestedName(name, it->name),
					*it->type,
					*column_nested.getData()[i],
					column_nested.getOffsets()[column.size() - 1],
					level + 1);
			}
		}
	}
	else
		type.deserializeBinary(column, streams[name]->compressed, limit);
}


TinyLogBlockOutputStream::TinyLogBlockOutputStream(StorageTinyLog & storage_)
	: storage(storage_)
{
	for (NamesAndTypesList::const_iterator it = storage.columns->begin(); it != storage.columns->end(); ++it)
		addStream(it->name, *it->type);
}


void TinyLogBlockOutputStream::addStream(const String & name, const IDataType & type, size_t level)
{
	/// Для массивов используются отдельные потоки для размеров.
	if (const DataTypeArray * type_arr = typeid_cast<const DataTypeArray *>(&type))
	{
		String size_name = DataTypeNested::extractNestedTableName(name) + ARRAY_SIZES_COLUMN_NAME_SUFFIX + toString(level);
		if (!streams.count(size_name))
			streams.emplace(size_name, std::unique_ptr<Stream>(new Stream(storage.files[size_name].data_file.path(), storage.max_compress_block_size)));

		addStream(name, *type_arr->getNestedType(), level + 1);
	}
	else if (const DataTypeNested * type_nested = typeid_cast<const DataTypeNested *>(&type))
	{
		String size_name = name + ARRAY_SIZES_COLUMN_NAME_SUFFIX + toString(level);
		streams[size_name].reset(new Stream(storage.files[size_name].data_file.path(), storage.max_compress_block_size));

		const NamesAndTypesList & columns = *type_nested->getNestedTypesList();
		for (NamesAndTypesList::const_iterator it = columns.begin(); it != columns.end(); ++it)
			addStream(DataTypeNested::concatenateNestedName(name, it->name), *it->type, level + 1);
	}
	else
		streams[name].reset(new Stream(storage.files[name].data_file.path(), storage.max_compress_block_size));
}


void TinyLogBlockOutputStream::writeData(const String & name, const IDataType & type, const IColumn & column,
											OffsetColumns & offset_columns, size_t level)
{
	/// Для массивов требуется сначала сериализовать размеры, а потом значения.
	if (const DataTypeArray * type_arr = typeid_cast<const DataTypeArray *>(&type))
	{
		String size_name = DataTypeNested::extractNestedTableName(name) + ARRAY_SIZES_COLUMN_NAME_SUFFIX + toString(level);

		if (offset_columns.count(size_name) == 0)
		{
			offset_columns.insert(size_name);
			type_arr->serializeOffsets(
				column,
				streams[size_name]->compressed);
		}

		writeData(name, *type_arr->getNestedType(), typeid_cast<const ColumnArray &>(column).getData(), offset_columns, level + 1);
	}
	else if (const DataTypeNested * type_nested = typeid_cast<const DataTypeNested *>(&type))
	{
		String size_name = name + ARRAY_SIZES_COLUMN_NAME_SUFFIX + toString(level);

		type_nested->serializeOffsets(column, streams[size_name]->compressed);

		const ColumnNested & column_nested = typeid_cast<const ColumnNested &>(column);

		NamesAndTypesList::const_iterator it = type_nested->getNestedTypesList()->begin();
		for (size_t i = 0; i < column_nested.getData().size(); ++i, ++it)
		{
			writeData(
				DataTypeNested::concatenateNestedName(name, it->name),
				*it->type,
				*column_nested.getData()[i],
				offset_columns,
				level + 1);
		}
	}
	else
		type.serializeBinary(column, streams[name]->compressed);
}


void TinyLogBlockOutputStream::writeSuffix()
{
	/// Заканчиваем запись.
	for (FileStreams::iterator it = streams.begin(); it != streams.end(); ++it)
		it->second->finalize();

	streams.clear();
}


void TinyLogBlockOutputStream::write(const Block & block)
{
	storage.check(block, true);

	/// Множество записанных столбцов со смещениями, чтобы не писать общие для вложенных структур столбцы несколько раз
	OffsetColumns offset_columns;

	for (size_t i = 0; i < block.columns(); ++i)
	{
		const ColumnWithNameAndType & column = block.getByPosition(i);
		writeData(column.name, *column.type, *column.column, offset_columns);
	}
}


StorageTinyLog::StorageTinyLog(const std::string & path_, const std::string & name_, NamesAndTypesListPtr columns_, bool attach, size_t max_compress_block_size_)
	: path(path_), name(name_), columns(columns_), max_compress_block_size(max_compress_block_size_)
{
	if (columns->empty())
		throw Exception("Empty list of columns passed to StorageTinyLog constructor", ErrorCodes::EMPTY_LIST_OF_COLUMNS_PASSED);

	if (!attach)
	{
		/// создаём файлы, если их нет
		String full_path = path + escapeForFileName(name) + '/';
		if (0 != mkdir(full_path.c_str(), S_IRWXU | S_IRWXG | S_IRWXO) && errno != EEXIST)
			throwFromErrno("Cannot create directory " + full_path, ErrorCodes::CANNOT_CREATE_DIRECTORY);
	}

	for (NamesAndTypesList::const_iterator it = columns->begin(); it != columns->end(); ++it)
		addFile(it->name, *it->type);
}

StoragePtr StorageTinyLog::create(const std::string & path_, const std::string & name_, NamesAndTypesListPtr columns_, bool attach, size_t max_compress_block_size_)
{
	return (new StorageTinyLog(path_, name_, columns_, attach, max_compress_block_size_))->thisPtr();
}


void StorageTinyLog::addFile(const String & column_name, const IDataType & type, size_t level)
{
	if (files.end() != files.find(column_name))
		throw Exception("Duplicate column with name " + column_name + " in constructor of StorageTinyLog.",
			ErrorCodes::DUPLICATE_COLUMN);

	if (const DataTypeArray * type_arr = typeid_cast<const DataTypeArray *>(&type))
	{
		String size_column_suffix = ARRAY_SIZES_COLUMN_NAME_SUFFIX + toString(level);
		String size_name = DataTypeNested::extractNestedTableName(column_name) + size_column_suffix;

		if (files.end() == files.find(size_name))
		{
			ColumnData column_data;
			files.insert(std::make_pair(size_name, column_data));
			files[size_name].data_file = Poco::File(
				path + escapeForFileName(name) + '/' + escapeForFileName(DataTypeNested::extractNestedTableName(column_name)) + size_column_suffix + DBMS_STORAGE_LOG_DATA_FILE_EXTENSION);
		}

		addFile(column_name, *type_arr->getNestedType(), level + 1);
	}
	else if (const DataTypeNested * type_nested = typeid_cast<const DataTypeNested *>(&type))
	{
		String size_column_suffix = ARRAY_SIZES_COLUMN_NAME_SUFFIX + toString(level);

		ColumnData column_data;
		files.insert(std::make_pair(column_name + size_column_suffix, column_data));
		files[column_name + size_column_suffix].data_file = Poco::File(
			path + escapeForFileName(name) + '/' + escapeForFileName(column_name) + size_column_suffix + DBMS_STORAGE_LOG_DATA_FILE_EXTENSION);

		const NamesAndTypesList & columns = *type_nested->getNestedTypesList();
		for (NamesAndTypesList::const_iterator it = columns.begin(); it != columns.end(); ++it)
			addFile(DataTypeNested::concatenateNestedName(name, it->name), *it->type, level + 1);
	}
	else
	{
		ColumnData column_data;
		files.insert(std::make_pair(column_name, column_data));
		files[column_name].data_file = Poco::File(
			path + escapeForFileName(name) + '/' + escapeForFileName(column_name) + DBMS_STORAGE_LOG_DATA_FILE_EXTENSION);
	}
}


void StorageTinyLog::rename(const String & new_path_to_db, const String & new_name)
{
	/// Переименовываем директорию с данными.
	Poco::File(path + escapeForFileName(name)).renameTo(new_path_to_db + escapeForFileName(new_name));

	path = new_path_to_db;
	name = new_name;

	for (Files_t::iterator it = files.begin(); it != files.end(); ++it)
		it->second.data_file = Poco::File(path + escapeForFileName(name) + '/' + Poco::Path(it->second.data_file.path()).getFileName());
}


BlockInputStreams StorageTinyLog::read(
	const Names & column_names,
	ASTPtr query,
	const Settings & settings,
	QueryProcessingStage::Enum & processed_stage,
	size_t max_block_size,
	unsigned threads)
{
	check(column_names);
	processed_stage = QueryProcessingStage::FetchColumns;
	return BlockInputStreams(1, new TinyLogBlockInputStream(max_block_size, column_names, *this));
}


BlockOutputStreamPtr StorageTinyLog::write(
	ASTPtr query)
{
	return new TinyLogBlockOutputStream(*this);
}


void StorageTinyLog::drop()
{
	for (Files_t::iterator it = files.begin(); it != files.end(); ++it)
		if (it->second.data_file.exists())
			it->second.data_file.remove();
}

}
