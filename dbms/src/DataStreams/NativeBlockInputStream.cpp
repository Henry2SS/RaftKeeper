#include <DB/Core/Defines.h>

#include <DB/IO/ReadHelpers.h>
#include <DB/IO/VarInt.h>

#include <DB/Columns/ColumnArray.h>
#include <DB/Columns/ColumnNested.h>

#include <DB/DataTypes/DataTypeArray.h>
#include <DB/DataTypes/DataTypeNested.h>

#include <DB/DataStreams/NativeBlockInputStream.h>


namespace DB
{


static void readData(const IDataType & type, IColumn & column, ReadBuffer & istr, size_t rows)
{
	/** Для массивов требуется сначала десериализовать смещения, а потом значения.
	  */
	if (const DataTypeArray * type_arr = dynamic_cast<const DataTypeArray *>(&type))
	{
		IColumn & offsets_column = *dynamic_cast<ColumnArray &>(column).getOffsetsColumn();
		type_arr->getOffsetsType()->deserializeBinary(offsets_column, istr, rows);

		if (offsets_column.size() != rows)
			throw Exception("Cannot read all data in NativeBlockInputStream.", ErrorCodes::CANNOT_READ_ALL_DATA);

		if (rows)
			readData(
				*type_arr->getNestedType(),
				dynamic_cast<ColumnArray &>(column).getData(),
				istr,
				dynamic_cast<const ColumnArray &>(column).getOffsets()[rows - 1]);
	}
	else if (const DataTypeNested * type_nested = dynamic_cast<const DataTypeNested *>(&type))
	{
		ColumnNested & column_nested = dynamic_cast<ColumnNested &>(column);
		IColumn & offsets_column = *column_nested.getOffsetsColumn();
		type_nested->getOffsetsType()->deserializeBinary(offsets_column, istr, rows);

		if (offsets_column.size() != rows)
			throw Exception("Cannot read all data in NativeBlockInputStream.", ErrorCodes::CANNOT_READ_ALL_DATA);

		if (rows)
		{
			NamesAndTypesList::const_iterator it = type_nested->getNestedTypesList()->begin();
			for (size_t i = 0; i < column_nested.getData().size(); ++i, ++it)
			{
				readData(
					*it->second,
					*column_nested.getData()[i],
					istr,
					column_nested.getOffsets()[rows - 1]);
			}
		}
	}
	else
		type.deserializeBinary(column, istr, rows);

	if (column.size() != rows)
		throw Exception("Cannot read all data in NativeBlockInputStream.", ErrorCodes::CANNOT_READ_ALL_DATA);
}


Block NativeBlockInputStream::readImpl()
{
	Block res;

	if (istr.eof())
		return res;
	
	/// Размеры
	size_t columns = 0;
	size_t rows = 0;
	readVarUInt(columns, istr);
	readVarUInt(rows, istr);

	for (size_t i = 0; i < columns; ++i)
	{
		ColumnWithNameAndType column;

		/// Имя
		readStringBinary(column.name, istr);

		/// Тип
		String type_name;
		readStringBinary(type_name, istr);
		column.type = data_type_factory.get(type_name);

		/// Данные
		column.column = column.type->createColumn();
		readData(*column.type, *column.column, istr, rows);

		res.insert(column);
	}

	return res;
}

}
