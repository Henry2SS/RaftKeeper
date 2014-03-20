#include <DB/Storages/StorageChunkMerger.h>
#include <DB/Storages/StorageChunks.h>
#include <DB/Storages/StorageChunkRef.h>
#include <DB/Parsers/ASTCreateQuery.h>
#include <DB/Parsers/ASTNameTypePair.h>
#include <DB/Parsers/ASTLiteral.h>
#include <DB/Parsers/ASTIdentifier.h>
#include <DB/Parsers/ASTSelectQuery.h>
#include <DB/Parsers/ASTDropQuery.h>
#include <DB/Parsers/ParserCreateQuery.h>
#include <DB/Parsers/formatAST.h>
#include <DB/Interpreters/executeQuery.h>
#include <DB/Interpreters/InterpreterDropQuery.h>
#include <DB/DataStreams/copyData.h>
#include <DB/DataStreams/ConcatBlockInputStream.h>
#include <DB/DataStreams/narrowBlockInputStreams.h>
#include <DB/DataStreams/AddingDefaultBlockInputStream.h>
#include <DB/Common/VirtualColumnUtils.h>


namespace DB
{
	
const int SLEEP_AFTER_MERGE = 1;
const int SLEEP_NO_WORK = 10;
const int SLEEP_AFTER_ERROR = 60;

StorageChunkMerger::TableNames StorageChunkMerger::currently_written_groups;

StoragePtr StorageChunkMerger::create(
	const std::string & this_database_,
	const std::string & name_,
	NamesAndTypesListPtr columns_,
	const String & source_database_,
	const String & table_name_regexp_,
	const std::string & destination_name_prefix_,
	size_t chunks_to_merge_,
	Context & context_)
{
	return (new StorageChunkMerger(this_database_, name_, columns_, source_database_, table_name_regexp_, destination_name_prefix_, chunks_to_merge_, context_))->thisPtr();
}

NameAndTypePair StorageChunkMerger::getColumn(const String &column_name) const
{
	if (column_name == _table_column_name) return std::make_pair(_table_column_name, new DataTypeString);
	return getRealColumn(column_name);
}

bool StorageChunkMerger::hasColumn(const String &column_name) const
{
	if (column_name == _table_column_name) return true;
	return hasRealColumn(column_name);
}

BlockInputStreams StorageChunkMerger::read(
	const Names & column_names,
	ASTPtr query,
	const Settings & settings,
	QueryProcessingStage::Enum & processed_stage,
	size_t max_block_size,
	unsigned threads)
{
	/// Будем читать из таблиц Chunks, на которые есть хоть одна ChunkRef, подходящая под регэксп, и из прочих таблиц, подходящих под регэксп.
	Storages selected_tables;

	{
		Poco::ScopedLock<Poco::Mutex> lock(context.getMutex());
		
		typedef std::set<std::string> StringSet;
		StringSet chunks_table_names;
		
		Databases & databases = context.getDatabases();
		
		if (!databases.count(source_database))
			throw Exception("No database " + source_database, ErrorCodes::UNKNOWN_DATABASE);
		
		Tables & tables = databases[source_database];
		for (Tables::iterator it = tables.begin(); it != tables.end(); ++it)
		{
			StoragePtr table = it->second;
			if (table_name_regexp.match(it->first) &&
				!dynamic_cast<StorageChunks *>(&*table) &&
				!dynamic_cast<StorageChunkMerger *>(&*table))
			{
				if (StorageChunkRef * chunk_ref = dynamic_cast<StorageChunkRef *>(&*table))
				{
					if (chunk_ref->source_database_name != source_database)
					{
						LOG_WARNING(log, "ChunkRef " + chunk_ref->getTableName() + " points to another database, ignoring");
						continue;
					}
					if (!chunks_table_names.count(chunk_ref->source_table_name))
					{
						if (tables.count(chunk_ref->source_table_name))
						{
							chunks_table_names.insert(chunk_ref->source_table_name);
							selected_tables.push_back(tables[chunk_ref->source_table_name]);
						}
						else
						{
							LOG_WARNING(log, "ChunkRef " + chunk_ref->getTableName() + " points to non-existing Chunks table, ignoring");
						}
					}
				}
				else
				{
					selected_tables.push_back(table);
				}
			}
		}
	}

	BlockInputStreams res;
	
	/// Среди всех стадий, до которых обрабатывается запрос в таблицах-источниках, выберем минимальную.
	processed_stage = QueryProcessingStage::Complete;
	QueryProcessingStage::Enum tmp_processed_stage = QueryProcessingStage::Complete;

	bool has_virtual_column = false;

	for (const auto & column : column_names)
		if (column == _table_column_name)
			has_virtual_column = true;

	Block virtual_columns_block = getBlockWithVirtualColumns(selected_tables);
	BlockInputStreamPtr virtual_columns;

	/// Если запрошен хотя бы один виртуальный столбец, пробуем индексировать
	if (has_virtual_column)
		virtual_columns = VirtualColumnUtils::getVirtualColumnsBlocks(query->clone(), virtual_columns_block, context);
	else /// Иначе, считаем допустимыми все возможные значения
		virtual_columns = new OneBlockInputStream(virtual_columns_block);

	std::set<String> values = VirtualColumnUtils::extractSingleValueFromBlocks<String>(virtual_columns, _table_column_name);
	bool all_inclusive = (values.size() == virtual_columns_block.rows());
	
	for (Storages::iterator it = selected_tables.begin(); it != selected_tables.end(); ++it)
	{
		if ((*it)->getName() != "Chunks" && !all_inclusive && values.find((*it)->getTableName()) == values.end())
			continue;

		/// Список виртуальных столбцов, которые мы заполним сейчас и список столбцов, которые передадим дальше
		Names virt_column_names, real_column_names;
		for (const auto & column : column_names)
			if (column == _table_column_name && (*it)->getName() != "Chunks") /// таблица Chunks сама заполняет столбец _table
				virt_column_names.push_back(column);
			else
				real_column_names.push_back(column);

		/// Если в запросе только виртуальные столбцы, надо запросить хотя бы один любой другой.
		if (real_column_names.size() == 0)
			real_column_names.push_back(ExpressionActions::getSmallestColumn((*it)->getColumnsList()));

		ASTPtr modified_query_ast = query->clone();

		/// Подменяем виртуальный столбец на его значение
		if (!virt_column_names.empty())
			VirtualColumnUtils::rewriteEntityInAst(modified_query_ast, _table_column_name, (*it)->getTableName());

		BlockInputStreams source_streams = (*it)->read(
			real_column_names,
			modified_query_ast,
			settings,
			tmp_processed_stage,
			max_block_size,
			selected_tables.size() > threads ? 1 : (threads / selected_tables.size()));

		/// Добавляем в ответ вирутальные столбцы
		for (const auto & virtual_column : virt_column_names)
		{
			if (virtual_column == _table_column_name)
			{
				for (auto & stream : source_streams)
				{
					stream = new AddingConstColumnBlockInputStream<String>(stream, new DataTypeString, (*it)->getTableName(), _table_column_name);
				}
			}
		}

		for (BlockInputStreams::iterator jt = source_streams.begin(); jt != source_streams.end(); ++jt)
			res.push_back(*jt);
		
		if (tmp_processed_stage < processed_stage)
			processed_stage = tmp_processed_stage;
	}
	
	/** Если истчоников слишком много, то склеим их в threads источников.
	 */
	if (res.size() > threads)
		res = narrowBlockInputStreams(res, threads);
	
	return res;
}

/// Построить блок состоящий только из возможных значений виртуальных столбцов
Block StorageChunkMerger::getBlockWithVirtualColumns(const Storages & selected_tables) const
{
	Block res;
	ColumnWithNameAndType _table(new ColumnString, new DataTypeString, _table_column_name);

	for (Storages::const_iterator it = selected_tables.begin(); it != selected_tables.end(); ++it)
		if ((*it)->getName() != "Chunks")
			_table.column->insert((*it)->getTableName());

	res.insert(_table);
	return res;
}

StorageChunkMerger::StorageChunkMerger(
	const std::string & this_database_,
	const std::string & name_,
	NamesAndTypesListPtr columns_,
	const String & source_database_,
	const String & table_name_regexp_,
	const std::string & destination_name_prefix_,
	size_t chunks_to_merge_,
	Context & context_)
	: this_database(this_database_), name(name_), columns(columns_), source_database(source_database_),
	table_name_regexp(table_name_regexp_), destination_name_prefix(destination_name_prefix_), chunks_to_merge(chunks_to_merge_),
	context(context_), settings(context.getSettings()),
	log(&Logger::get("StorageChunkMerger")), shutdown_called(false)
{
	merge_thread = boost::thread(&StorageChunkMerger::mergeThread, this);
	_table_column_name = "_table" + VirtualColumnUtils::chooseSuffix(getColumnsList(), "_table");
}

void StorageChunkMerger::shutdown()
{
	if (shutdown_called)
		return;
	shutdown_called = true;

	cancel_merge_thread.set();
	merge_thread.join();
}

StorageChunkMerger::~StorageChunkMerger()
{
	shutdown();
}

void StorageChunkMerger::mergeThread()
{
	while (true)
	{
		bool merged = false;
		bool error = true;

		try
		{
			merged = maybeMergeSomething();
			error = false;
		}
		catch (const Exception & e)
		{
			LOG_ERROR(log, "StorageChunkMerger at " << this_database << "." << name << " failed to merge: Code: " << e.code() << ", e.displayText() = " << e.displayText() << ", e.what() = " << e.what()
			<< ", Stack trace:\n\n" << e.getStackTrace().toString());
		}
		catch (const Poco::Exception & e)
		{
			LOG_ERROR(log, "StorageChunkMerger at " << this_database << "." << name << " failed to merge: Poco::Exception. Code: " << ErrorCodes::POCO_EXCEPTION << ", e.code() = " << e.code()
			<< ", e.displayText() = " << e.displayText() << ", e.what() = " << e.what());
		}
		catch (const std::exception & e)
		{
			LOG_ERROR(log, "StorageChunkMerger at " << this_database << "." << name << " failed to merge: std::exception. Code: " << ErrorCodes::STD_EXCEPTION << ", e.what() = " << e.what());
		}
		catch (...)
		{
			LOG_ERROR(log, "StorageChunkMerger at " << this_database << "." << name << " failed to merge: unknown exception. Code: " << ErrorCodes::UNKNOWN_EXCEPTION);
		}
		
		unsigned sleep_ammount = error ? SLEEP_AFTER_ERROR : (merged ? SLEEP_AFTER_MERGE : SLEEP_NO_WORK);
		if (shutdown_called || cancel_merge_thread.tryWait(1000 * sleep_ammount))
			break;
	}
}

static std::string makeName(const std::string & prefix, const std::string & first_chunk, const std::string & last_chunk)
{
	size_t lcp = 0; /// Длина общего префикса имен чанков.
	while (lcp < first_chunk.size() && lcp < last_chunk.size() && first_chunk[lcp] == last_chunk[lcp])
		++lcp;
	return prefix + first_chunk + "_" + last_chunk.substr(lcp);
}

bool StorageChunkMerger::maybeMergeSomething()
{
	Storages chunks = selectChunksToMerge();
	if (chunks.empty() || shutdown_called)
		return false;
	return mergeChunks(chunks);
}

StorageChunkMerger::Storages StorageChunkMerger::selectChunksToMerge()
{
	Poco::ScopedLock<Poco::Mutex> lock(context.getMutex());

	Storages res;

	Databases & databases = context.getDatabases();
	
	if (!databases.count(source_database))
		throw Exception("No database " + source_database, ErrorCodes::UNKNOWN_DATABASE);

	Tables & tables = databases[source_database];
	for (Tables::iterator it = tables.begin(); it != tables.end(); ++it)
	{
		StoragePtr table = it->second;
		if (table_name_regexp.match(it->first) &&
			!dynamic_cast<StorageChunks *>(&*table) &&
			!dynamic_cast<StorageChunkMerger *>(&*table) &&
			!dynamic_cast<StorageChunkRef *>(&*table))
		{
			res.push_back(table);
			
			if (res.size() >= chunks_to_merge)
				break;
		}
	}

	if (res.size() < chunks_to_merge)
		res.clear();
	
	return res;
}

static ASTPtr newIdentifier(const std::string & name, ASTIdentifier::Kind kind)
{
	ASTIdentifier * res = new ASTIdentifier;
	res->name = name;
	res->kind = kind;
	return res;
}

bool StorageChunkMerger::mergeChunks(const Storages & chunks)
{
	typedef std::map<std::string, DataTypePtr> ColumnsMap;
	
	/// Объединим множества столбцов сливаемых чанков.
	ColumnsMap known_columns_types(columns->begin(), columns->end());
	NamesAndTypesListPtr required_columns = new NamesAndTypesList;
	*required_columns = *columns;
	
	for (size_t chunk_index = 0; chunk_index < chunks.size(); ++chunk_index)
	{
		const NamesAndTypesList & current_columns = chunks[chunk_index]->getColumnsList();
		
		for (NamesAndTypesList::const_iterator it = current_columns.begin(); it != current_columns.end(); ++it)
		{
			const std::string & name = it->first;
			const DataTypePtr & type = it->second;
			if (known_columns_types.count(name))
			{
				String current_type_name = type->getName();
				String known_type_name = known_columns_types[name]->getName();
				if (current_type_name != known_type_name)
					throw Exception("Different types of column " + name + " in different chunks: type " + current_type_name + " in chunk " + chunks[chunk_index]->getTableName() + ", type " + known_type_name + " somewhere else", ErrorCodes::TYPE_MISMATCH);
			}
			else
			{
				known_columns_types[name] = type;
				required_columns->push_back(*it);
			}
		}
	}
	
	std::string formatted_columns = formatColumnsForCreateQuery(*required_columns);
	
	std::string new_table_name = makeName(destination_name_prefix, chunks.front()->getTableName(), chunks.back()->getTableName());
	std::string new_table_full_name = backQuoteIfNeed(source_database) + "." + backQuoteIfNeed(new_table_name);
	StoragePtr new_storage_ptr;
	
	try
	{
		{
			Poco::ScopedLock<Poco::Mutex> lock(context.getMutex());
			
			if (!context.getDatabases().count(source_database))
				throw Exception("Destination database " + source_database + " for table " + name + " doesn't exist", ErrorCodes::UNKNOWN_DATABASE);
			
			LOG_TRACE(log, "Will merge " << chunks.size() << " chunks: from " << chunks[0]->getTableName() << " to " << chunks.back()->getTableName() << " to new table " << new_table_name << ".");
			
			if (currently_written_groups.count(new_table_full_name))
			{
				LOG_WARNING(log, "Table " + new_table_full_name + " is already being written. Aborting merge.");
				return false;
			}
			
			currently_written_groups.insert(new_table_full_name);
		}
			
		/// Уроним Chunks таблицу с таким именем, если она есть. Она могла остаться в результате прерванного слияния той же группы чанков.
		executeQuery("DROP TABLE IF EXISTS " + new_table_full_name, context, true);

		/// Выполним запрос для создания Chunks таблицы.
		executeQuery("CREATE TABLE " + new_table_full_name + " " + formatted_columns + " ENGINE = Chunks", context, true);

		new_storage_ptr = context.getTable(source_database, new_table_name);

		/// Скопируем данные в новую таблицу.
		StorageChunks & new_storage = dynamic_cast<StorageChunks &>(*new_storage_ptr);
		
		for (size_t chunk_index = 0; chunk_index < chunks.size(); ++chunk_index)
		{
			StoragePtr src_storage = chunks[chunk_index];
			BlockOutputStreamPtr output = new_storage.writeToNewChunk(src_storage->getTableName());
			
			const NamesAndTypesList & src_columns = src_storage->getColumnsList();
			Names src_column_names;
			
			ASTSelectQuery * select_query = new ASTSelectQuery;
			ASTPtr select_query_ptr = select_query;
			
			/// Запрос, вынимающий нужные столбцы.
			ASTPtr select_expression_list;
			ASTPtr database;
			ASTPtr table;	/// Идентификатор или подзапрос (рекурсивно ASTSelectQuery)
			select_query->database = newIdentifier(source_database, ASTIdentifier::Database);
			select_query->table = newIdentifier(src_storage->getTableName(), ASTIdentifier::Table);
			ASTExpressionList * select_list = new ASTExpressionList;
			select_query->select_expression_list = select_list;
			for (NamesAndTypesList::const_iterator it = src_columns.begin(); it != src_columns.end(); ++it)
			{
				src_column_names.push_back(it->first);
				select_list->children.push_back(newIdentifier(it->first, ASTIdentifier::Column));
			}
			
			QueryProcessingStage::Enum processed_stage = QueryProcessingStage::Complete;

			BlockInputStreams input_streams = src_storage->read(
				src_column_names,
				select_query_ptr,
				settings,
				processed_stage,
				DEFAULT_MERGE_BLOCK_SIZE);
			
			BlockInputStreamPtr input = new AddingDefaultBlockInputStream(new ConcatBlockInputStream(input_streams), required_columns);
			
			input->readPrefix();
			output->writePrefix();

			Block block;
			while (!shutdown_called && (block = input->read()))
				output->write(block);

			if (shutdown_called)
			{
				LOG_INFO(log, "Shutdown requested while merging chunks.");
				new_storage.removeReference();	/// После этого временные данные удалятся.
				return false;
			}

			input->readSuffix();
			output->writeSuffix();
		}
		
		/// Атомарно подменим исходные таблицы ссылками на новую.
		/// При этом удалять таблицы под мьютексом контекста нельзя, пока только отцепим их.
		Storages tables_to_drop;

		{
			Poco::ScopedLock<Poco::Mutex> lock(context.getMutex());
			
			/// Если БД успели удалить, ничего не делаем.
			if (context.getDatabases().count(source_database))
			{
				for (size_t chunk_index = 0; chunk_index < chunks.size(); ++chunk_index)
				{
					StoragePtr src_storage = chunks[chunk_index];
					std::string src_name = src_storage->getTableName();
					
					/// Если таблицу успели удалить, ничего не делаем.
					if (!context.isTableExist(source_database, src_name))
						continue;
					
					/// Отцепляем исходную таблицу. Ее данные и метаданные остаются на диске.
					tables_to_drop.push_back(context.detachTable(source_database, src_name));

					/// Создаем на ее месте ChunkRef. Это возможно только потому что у ChunkRef нет ни, ни метаданных.
					try
					{
						context.addTable(source_database, src_name, StorageChunkRef::create(src_name, context, source_database, new_table_name, false));
					}
					catch (...)
					{
						LOG_ERROR(log, "Chunk " + src_name + " was removed but not replaced. Its data is stored in table " << new_table_name << ". You may need to resolve this manually.");
						
						throw;
					}
				}
			}

			currently_written_groups.erase(new_table_full_name);
		}

		/// Теперь удалим данные отцепленных таблиц.
		for (StoragePtr table : tables_to_drop)
		{
			InterpreterDropQuery::dropDetachedTable(source_database, table, context);
			/// NOTE: Если между подменой таблицы и этой строчкой кто-то успеет попытаться создать новую таблицу на ее месте,
			///  что-нибудь может сломаться.
		}

		/// Сейчас на new_storage ссылаются таблицы типа ChunkRef. Удалим лишнюю ссылку, которая была при создании.
		new_storage.removeReference();

		LOG_TRACE(log, "Merged chunks.");
		
		return true;
	}
	catch(...)
	{
		Poco::ScopedLock<Poco::Mutex> lock(context.getMutex());
		
		currently_written_groups.erase(new_table_full_name);
		
		throw;
	}
}
	
}
