#include <DB/Columns/ColumnString.h>
#include <DB/DataTypes/DataTypeString.h>
#include <DB/DataStreams/OneBlockInputStream.h>
#include <DB/Storages/StorageSystemDatabases.h>


namespace DB
{


StorageSystemDatabases::StorageSystemDatabases(const std::string & name_, const Context & context_)
	: name(name_), context(context_)
{
	columns.push_back(NameAndTypePair("name", new DataTypeString));
}

StoragePtr StorageSystemDatabases::create(const std::string & name_, const Context & context_)
{
	return (new StorageSystemDatabases(name_, context_))->thisPtr();
}


BlockInputStreams StorageSystemDatabases::read(
	const Names & column_names, ASTPtr query, const Settings & settings,
	QueryProcessingStage::Enum & processed_stage, size_t max_block_size, unsigned threads)
{
	check(column_names);
	processed_stage = QueryProcessingStage::FetchColumns;

	Block block;
	
	ColumnWithNameAndType col_name;
	col_name.name = "name";
	col_name.type = new DataTypeString;
	col_name.column = new ColumnString;
	block.insert(col_name);

	Poco::ScopedLock<Poco::Mutex> lock(context.getMutex());

	for (Databases::const_iterator it = context.getDatabases().begin(); it != context.getDatabases().end(); ++it)
		col_name.column->insert(it->first);

	return BlockInputStreams(1, new OneBlockInputStream(block));
}


}
