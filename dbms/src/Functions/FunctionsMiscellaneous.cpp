#include <math.h>

#include <DB/Functions/FunctionFactory.h>
#include <DB/Functions/FunctionsArithmetic.h>
#include <DB/Functions/FunctionsMiscellaneous.h>


namespace DB
{


template <typename T>
static void numWidthVector(const PODArray<T> & a, PODArray<UInt64> & c)
{
	size_t size = a.size();
	for (size_t i = 0; i < size; ++i)
		if (a[i] >= 0)
			c[i] = a[i] ? 1 + log10(a[i]) : 1;
		else if (std::is_signed<T>::value && a[i] == std::numeric_limits<T>::min())
			c[i] = 2 + log10(std::numeric_limits<T>::max());
		else
			c[i] = 2 + log10(-a[i]);
}

template <typename T>
static void numWidthConstant(T a, UInt64 & c)
{
	if (a >= 0)
		c = a ? 1 + log10(a) : 1;
	else if (std::is_signed<T>::value && a == std::numeric_limits<T>::min())
		c = 2 + log10(std::numeric_limits<T>::max());
	else
		c = 2 + log10(-a);
}

inline UInt64 floatWidth(double x)
{
	/// Не быстро.
	unsigned size = WRITE_HELPERS_DEFAULT_FLOAT_PRECISION + 10;
	char tmp[size];	/// знаки, +0.0e+123\0
	int res = std::snprintf(tmp, size, "%.*g", WRITE_HELPERS_DEFAULT_FLOAT_PRECISION, x);

	if (res >= static_cast<int>(size) || res <= 0)
		throw Exception("Cannot print float or double number", ErrorCodes::CANNOT_PRINT_FLOAT_OR_DOUBLE_NUMBER);

	return res;
}

template <typename T>
static void floatWidthVector(const PODArray<T> & a, PODArray<UInt64> & c)
{
	size_t size = a.size();
	for (size_t i = 0; i < size; ++i)
		c[i] = floatWidth(a[i]);
}

template <typename T>
static void floatWidthConstant(T a, UInt64 & c)
{
	c = floatWidth(a);
}

template <> inline void numWidthVector<Float64>(const PODArray<Float64> & a, PODArray<UInt64> & c) { floatWidthVector(a, c); }
template <> inline void numWidthVector<Float32>(const PODArray<Float32> & a, PODArray<UInt64> & c) { floatWidthVector(a, c); }
template <> inline void numWidthConstant<Float64>(Float64 a, UInt64 & c) { floatWidthConstant(a, c); }
template <> inline void numWidthConstant<Float32>(Float32 a, UInt64 & c) { floatWidthConstant(a, c); }


static inline void stringWidthVector(const ColumnString::Chars_t & data, const ColumnString::Offsets_t & offsets, PODArray<UInt64> & res)
{
	size_t size = offsets.size();

	size_t prev_offset = 0;
	for (size_t i = 0; i < size; ++i)
	{
		res[i] = stringWidth(&data[prev_offset], &data[offsets[i] - 1]);
		prev_offset = offsets[i];
	}
}

static inline void stringWidthFixedVector(const ColumnString::Chars_t & data, size_t n, PODArray<UInt64> & res)
{
	size_t size = data.size() / n;
	for (size_t i = 0; i < size; ++i)
		res[i] = stringWidth(&data[i * n], &data[(i + 1) * n]);
}


namespace VisibleWidth
{
	template <typename T>
	static bool executeConstNumber(Block & block, const ColumnPtr & column, size_t result)
	{
		if (const ColumnConst<T> * col = typeid_cast<const ColumnConst<T> *>(&*column))
		{
			UInt64 res = 0;
			numWidthConstant(col->getData(), res);
			block.getByPosition(result).column = new ColumnConstUInt64(column->size(), res);
			return true;
		}
		else
			return false;
	}

	template <typename T>
	static bool executeNumber(Block & block, const ColumnPtr & column, size_t result)
	{
		if (const ColumnVector<T> * col = typeid_cast<const ColumnVector<T> *>(&*column))
		{
			ColumnUInt64 * res = new ColumnUInt64(column->size());
			numWidthVector(col->getData(), res->getData());
			block.getByPosition(result).column = res;
			return true;
		}
		else
			return false;
	}
}


void FunctionVisibleWidth::execute(Block & block, const ColumnNumbers & arguments, size_t result)
{
	const ColumnPtr column = block.getByPosition(arguments[0]).column;
	const DataTypePtr type = block.getByPosition(arguments[0]).type;
	size_t rows = column->size();

	if (typeid_cast<const DataTypeDate *>(&*type))
	{
		block.getByPosition(result).column = new ColumnConstUInt64(rows, strlen("0000-00-00"));
	}
	else if (typeid_cast<const DataTypeDateTime *>(&*type))
	{
		block.getByPosition(result).column = new ColumnConstUInt64(rows, strlen("0000-00-00 00:00:00"));
	}
	else if (VisibleWidth::executeConstNumber<UInt8>(block, column, result)
		|| VisibleWidth::executeConstNumber<UInt16>(block, column, result)
		|| VisibleWidth::executeConstNumber<UInt32>(block, column, result)
		|| VisibleWidth::executeConstNumber<UInt64>(block, column, result)
		|| VisibleWidth::executeConstNumber<Int8>(block, column, result)
		|| VisibleWidth::executeConstNumber<Int16>(block, column, result)
		|| VisibleWidth::executeConstNumber<Int32>(block, column, result)
		|| VisibleWidth::executeConstNumber<Int64>(block, column, result)
		|| VisibleWidth::executeConstNumber<Float32>(block, column, result)	/// TODO: правильная работа с float
		|| VisibleWidth::executeConstNumber<Float64>(block, column, result)
		|| VisibleWidth::executeNumber<UInt8>(block, column, result)
		|| VisibleWidth::executeNumber<UInt16>(block, column, result)
		|| VisibleWidth::executeNumber<UInt32>(block, column, result)
		|| VisibleWidth::executeNumber<UInt64>(block, column, result)
		|| VisibleWidth::executeNumber<Int8>(block, column, result)
		|| VisibleWidth::executeNumber<Int16>(block, column, result)
		|| VisibleWidth::executeNumber<Int32>(block, column, result)
		|| VisibleWidth::executeNumber<Int64>(block, column, result)
		|| VisibleWidth::executeNumber<Float32>(block, column, result)
		|| VisibleWidth::executeNumber<Float64>(block, column, result))
	{
	}
	else if (const ColumnString * col = typeid_cast<const ColumnString *>(&*column))
	{
		ColumnUInt64 * res = new ColumnUInt64(rows);
		stringWidthVector(col->getChars(), col->getOffsets(), res->getData());
		block.getByPosition(result).column = res;
	}
	else if (const ColumnFixedString * col = typeid_cast<const ColumnFixedString *>(&*column))
	{
		ColumnUInt64 * res = new ColumnUInt64(rows);
		stringWidthFixedVector(col->getChars(), col->getN(), res->getData());
		block.getByPosition(result).column = res;
	}
	else if (const ColumnConstString * col = typeid_cast<const ColumnConstString *>(&*column))
	{
		UInt64 res = 0;
		stringWidthConstant(col->getData(), res);
		block.getByPosition(result).column = new ColumnConstUInt64(rows, res);
	}
	else if (const ColumnArray * col = typeid_cast<const ColumnArray *>(&*column))
	{
		/// Вычисляем видимую ширину для значений массива.
		Block nested_block;
		ColumnWithNameAndType nested_values;
		nested_values.type = typeid_cast<const DataTypeArray &>(*type).getNestedType();
		nested_values.column = col->getDataPtr();
		nested_block.insert(nested_values);

		ColumnWithNameAndType nested_result;
		nested_result.type = new DataTypeUInt64;
		nested_block.insert(nested_result);

		ColumnNumbers nested_argument_numbers(1, 0);
		execute(nested_block, nested_argument_numbers, 1);

		/// Теперь суммируем и кладём в результат.
		ColumnUInt64 * res = new ColumnUInt64(rows);
		ColumnUInt64::Container_t & vec = res->getData();

		size_t additional_symbols = 0;	/// Кавычки.
		if (typeid_cast<const DataTypeDate *>(&*nested_values.type)
			|| typeid_cast<const DataTypeDateTime *>(&*nested_values.type)
			|| typeid_cast<const DataTypeString *>(&*nested_values.type)
			|| typeid_cast<const DataTypeFixedString *>(&*nested_values.type))
			additional_symbols = 2;

		if (ColumnUInt64 * nested_result_column = typeid_cast<ColumnUInt64 *>(&*nested_block.getByPosition(1).column))
		{
			ColumnUInt64::Container_t & nested_res = nested_result_column->getData();

			size_t j = 0;
			for (size_t i = 0; i < rows; ++i)
			{
				/** Если пустой массив - то два символа: [];
				  * если непустой - то сначала один символ [, и по одному лишнему символу на значение: , или ].
				  */
				vec[i] = j == col->getOffsets()[i] ? 2 : 1;

				for (; j < col->getOffsets()[i]; ++j)
					vec[i] += 1 + additional_symbols + nested_res[j];
			}
		}
		else if (ColumnConstUInt64 * nested_result_column = typeid_cast<ColumnConstUInt64 *>(&*nested_block.getByPosition(1).column))
		{
			size_t nested_length = nested_result_column->getData() + additional_symbols + 1;
			for (size_t i = 0; i < rows; ++i)
				vec[i] = 1 + std::max(static_cast<size_t>(1),
					(i == 0 ? col->getOffsets()[0] : (col->getOffsets()[i] - col->getOffsets()[i - 1])) * nested_length);
		}

		block.getByPosition(result).column = res;
	}
	else if (const ColumnTuple * col = typeid_cast<const ColumnTuple *>(&*column))
	{
		/// Посчитаем видимую ширину для каждого вложенного столбца по отдельности, и просуммируем.
		Block nested_block = col->getData();
		size_t columns = nested_block.columns();

		FunctionPlus func_plus;

		for (size_t i = 0; i < columns; ++i)
		{
			nested_block.getByPosition(i).type = static_cast<const DataTypeTuple &>(*type).getElements()[i];

			/** nested_block будет состоять из следующих столбцов:
			  * x1, x2, x3... , width1, width2, width1 + width2, width3, width1 + width2 + width3, ...
			  */

			ColumnWithNameAndType nested_result;
			nested_result.type = new DataTypeUInt64;
			nested_block.insert(nested_result);

			ColumnNumbers nested_argument_numbers(1, i);
			execute(nested_block, nested_argument_numbers, nested_block.columns() - 1);

			if (i != 0)
			{
				ColumnWithNameAndType plus_result;
				plus_result.type = new DataTypeUInt64;
				nested_block.insert(plus_result);

				ColumnNumbers plus_argument_numbers(2);
				plus_argument_numbers[0] = nested_block.columns() - 3;
				plus_argument_numbers[1] = nested_block.columns() - 2;
				func_plus.execute(nested_block, plus_argument_numbers, nested_block.columns() - 1);
			}
		}

		/// Прибавим ещё количество символов на кавычки и запятые.

		size_t additional_symbols = columns - 1;	/// Запятые.
		for (size_t i = 0; i < columns; ++i)
		{
			if (typeid_cast<const DataTypeDate *>(&*nested_block.getByPosition(i).type)
				|| typeid_cast<const DataTypeDateTime *>(&*nested_block.getByPosition(i).type)
				|| typeid_cast<const DataTypeString *>(&*nested_block.getByPosition(i).type)
				|| typeid_cast<const DataTypeFixedString *>(&*nested_block.getByPosition(i).type))
				additional_symbols += 2;			/// Кавычки.
		}

		ColumnUInt64 * nested_result_column = typeid_cast<ColumnUInt64 *>(&*nested_block.getByPosition(nested_block.columns() - 1).column);
		ColumnUInt64::Container_t & nested_res = nested_result_column->getData();

		for (size_t i = 0; i < rows; ++i)
			nested_res[i] += 2 + additional_symbols;

		block.getByPosition(result).column = nested_block.getByPosition(nested_block.columns() - 1).column;
	}
	else if (const ColumnConstArray * col = typeid_cast<const ColumnConstArray *>(&*column))
	{
		String s;
		{
			WriteBufferFromString wb(s);
			type->serializeTextEscaped(col->getData(), wb);
		}

		block.getByPosition(result).column = new ColumnConstUInt64(rows, s.size());
	}
	else
	   throw Exception("Illegal column " + block.getByPosition(arguments[0]).column->getName()
			+ " of argument of function " + getName(),
			ErrorCodes::ILLEGAL_COLUMN);
}

}


namespace DB
{

void registerFunctionsMiscellaneous(FunctionFactory & factory)
{
	#define F [](const Context & context)

	factory.registerFunction("hostName", 		F { return new FunctionHostName; });
	factory.registerFunction("visibleWidth", 	F { return new FunctionVisibleWidth; });
	factory.registerFunction("toTypeName", 		F { return new FunctionToTypeName; });
	factory.registerFunction("blockSize", 		F { return new FunctionBlockSize; });
	factory.registerFunction("sleep", 			F { return new FunctionSleep; });
	factory.registerFunction("materialize", 	F { return new FunctionMaterialize; });
	factory.registerFunction("ignore", 			F { return new FunctionIgnore; });
	factory.registerFunction("arrayJoin", 		F { return new FunctionArrayJoin; });
	factory.registerFunction("bar", 			F { return new FunctionBar; });

	factory.registerFunction("tuple", 			F { return new FunctionTuple; });
	factory.registerFunction("tupleElement", 	F { return new FunctionTupleElement; });
	factory.registerFunction("in", 				F { return new FunctionIn(false, false); });
	factory.registerFunction("notIn", 			F { return new FunctionIn(true, false); });
	factory.registerFunction("globalIn", 		F { return new FunctionIn(false, true); });
	factory.registerFunction("globalNotIn", 	F { return new FunctionIn(true, true); });

	#undef F
}

}
