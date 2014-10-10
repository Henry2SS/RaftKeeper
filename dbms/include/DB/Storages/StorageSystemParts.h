#pragma once

#include <Poco/SharedPtr.h>

#include <DB/Storages/IStorage.h>
#include <DB/Interpreters/Context.h>


namespace DB
{

using Poco::SharedPtr;


/** Реализует системную таблицу tables, которая позволяет получить информацию о всех таблицах.
  */
class StorageSystemParts : public IStorage
{
public:
	static StoragePtr create(const std::string & name_, const Context & context_);

	std::string getName() const { return "SystemParts"; }
	std::string getTableName() const { return name; }

	const NamesAndTypesList & getColumnsListImpl() const override { return columns; }

	BlockInputStreams read(
		const Names & column_names,
		ASTPtr query,
		const Settings & settings,
		QueryProcessingStage::Enum & processed_stage,
		size_t max_block_size = DEFAULT_BLOCK_SIZE,
		unsigned threads = 1);

private:
	const std::string name;
	const Context & context;
	NamesAndTypesList columns;

	StorageSystemParts(const std::string & name_, const Context & context_);
};

}
