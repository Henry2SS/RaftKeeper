#pragma once

#include <string.h>

#include <DB/Core/Exception.h>
#include <DB/Core/ErrorCodes.h>

#include <DB/Columns/IColumn.h>


namespace DB
{


/** Штука для сравнения чисел.
  * Целые числа сравниваются как обычно.
  * Числа с плавающей запятой сравниваются так, что NaN-ы всегда оказываются в конце
  *  (если этого не делать, то сортировка не работала бы вообще).
  */
template <typename T>
struct CompareHelper
{
	static bool less(T a, T b) { return a < b; }
	static bool greater(T a, T b) { return a > b; }

	/** Сравнивает два числа. Выдаёт число меньше нуля, равное нулю, или больше нуля, если a < b, a == b, a > b, соответственно.
	  * Если одно из значений является NaN, то:
	  * - если nan_direction_hint == -1 - NaN считаются меньше всех чисел;
	  * - если nan_direction_hint == 1 - NaN считаются больше всех чисел;
	  * По-сути: nan_direction_hint == -1 говорит, что сравнение идёт для сортировки по убыванию.
	  */
	static int compare(T a, T b, int nan_direction_hint) { return a - b; }
};

template <typename T>
struct FloatCompareHelper
{
	static bool less(T a, T b)
	{
		if (unlikely(isnan(b)))
			return !isnan(a);
		return a < b;
	}

	static bool greater(T a, T b)
	{
		if (unlikely(isnan(b)))
			return !isnan(a);
		return a > b;
	}

	static int compare(T a, T b, int nan_direction_hint)
	{
		bool isnan_a = isnan(a);
		bool isnan_b = isnan(b);
		if (unlikely(isnan_a || isnan_b))
		{
			if (isnan_a && isnan_b)
				return 0;

			return isnan_a
				? nan_direction_hint
				: -nan_direction_hint;
		}

		return (T(0) < (a - b)) - ((a - b) < T(0));
	}
};

template <> struct CompareHelper<Float32> : public FloatCompareHelper<Float32> {};
template <> struct CompareHelper<Float64> : public FloatCompareHelper<Float64> {};


/** Шаблон столбцов, которые используют для хранения std::vector.
  */
template <typename T>
class ColumnVectorBase : public IColumn
{
private:
	typedef ColumnVectorBase<T> Self;
public:
	typedef T value_type;
	typedef std::vector<value_type> Container_t;

	ColumnVectorBase() {}
	ColumnVectorBase(size_t n) : data(n) {}

	bool isNumeric() const { return IsNumber<T>::value; }
	bool isFixed() const { return IsNumber<T>::value; }

	size_t sizeOfField() const { return sizeof(T); }

	size_t size() const
	{
		return data.size();
	}

	StringRef getDataAt(size_t n) const
	{
		return StringRef(reinterpret_cast<const char *>(&data[n]), sizeof(data[n]));
	}
	
	void insertFrom(const IColumn & src, size_t n)
	{
		data.push_back(static_cast<const Self &>(src).getData()[n]);
	}

	void insertData(const char * pos, size_t length)
	{
		data.push_back(*reinterpret_cast<const T *>(pos));
	}

	void insertDefault()
	{
		data.push_back(T());
	}

	size_t byteSize() const
	{
		return data.size() * sizeof(data[0]);
	}

	int compareAt(size_t n, size_t m, const IColumn & rhs_, int nan_direction_hint) const
	{
		return CompareHelper<T>::compare(data[n], static_cast<const Self &>(rhs_).data[m], nan_direction_hint);
	}

	struct less
	{
		const Self & parent;
		less(const Self & parent_) : parent(parent_) {}
		bool operator()(size_t lhs, size_t rhs) const { return CompareHelper<T>::less(parent.data[lhs], parent.data[rhs]); }
	};

	struct greater
	{
		const Self & parent;
		greater(const Self & parent_) : parent(parent_) {}
		bool operator()(size_t lhs, size_t rhs) const { return CompareHelper<T>::greater(parent.data[lhs], parent.data[rhs]); }
	};

	Permutation getPermutation(bool reverse, size_t limit) const
	{
		size_t s = data.size();
		Permutation res(s);
		for (size_t i = 0; i < s; ++i)
			res[i] = i;

		if (limit > s)
			limit = 0;

		if (limit)
		{
			if (reverse)
				std::partial_sort(res.begin(), res.begin() + limit, res.end(), greater(*this));
			else
				std::partial_sort(res.begin(), res.begin() + limit, res.end(), less(*this));
		}
		else
		{
			if (reverse)
				std::sort(res.begin(), res.end(), greater(*this));
			else
				std::sort(res.begin(), res.end(), less(*this));
		}
		
		return res;
	}

	void reserve(size_t n)
	{
		data.reserve(n);
	}

	/** Более эффективные методы манипуляции */
	Container_t & getData()
	{
		return data;
	}

	const Container_t & getData() const
	{
		return data;
	}

protected:
	Container_t data;
};


/** Реализация для числовых типов.
  * (Есть ещё ColumnAggregateFunction.)
  */
template <typename T>
class ColumnVector : public ColumnVectorBase<T>
{
private:
	typedef ColumnVector<T> Self;
public:
	ColumnVector() {}
	ColumnVector(size_t n) : ColumnVectorBase<T>(n) {}

 	std::string getName() const { return "ColumnVector<" + TypeName<T>::get() + ">"; }

	ColumnPtr cloneEmpty() const
	{
		return new ColumnVector<T>;
	}

	Field operator[](size_t n) const
	{
		return typename NearestFieldType<T>::Type(this->data[n]);
	}

	void get(size_t n, Field & res) const
	{
		res = typename NearestFieldType<T>::Type(this->data[n]);
	}

	void insert(const Field & x)
	{
		this->data.push_back(DB::get<typename NearestFieldType<T>::Type>(x));
	}

	ColumnPtr cut(size_t start, size_t length) const
	{
		if (start + length > this->data.size())
			throw Exception("Parameters start = "
				+ toString(start) + ", length = "
				+ toString(length) + " are out of bound in IColumnVector<T>::cut() method"
				" (data.size() = " + toString(this->data.size()) + ").",
				ErrorCodes::PARAMETER_OUT_OF_BOUND);

		Self * res = new Self(length);
		memcpy(&res->getData()[0], &this->data[start], length * sizeof(this->data[0]));
		return res;
	}

	ColumnPtr filter(const IColumn::Filter & filt) const
	{
		size_t size = this->data.size();
		if (size != filt.size())
			throw Exception("Size of filter doesn't match size of column.", ErrorCodes::SIZES_OF_COLUMNS_DOESNT_MATCH);

		Self * res_ = new Self;
		ColumnPtr res = res_;
		typename Self::Container_t & res_data = res_->getData();
		res_data.reserve(size);

		for (size_t i = 0; i < size; ++i)
			if (filt[i])
				res_data.push_back(this->data[i]);

		return res;
	}

	ColumnPtr permute(const IColumn::Permutation & perm, size_t limit) const
	{
		size_t size = this->data.size();

		if (limit == 0)
			limit = size;
		else
			limit = std::min(size, limit);
		
		if (perm.size() < limit)
			throw Exception("Size of permutation is less than required.", ErrorCodes::SIZES_OF_COLUMNS_DOESNT_MATCH);

		Self * res_ = new Self(limit);
		ColumnPtr res = res_;
		typename Self::Container_t & res_data = res_->getData();
		for (size_t i = 0; i < limit; ++i)
			res_data[i] = this->data[perm[i]];

		return res;
	}

	ColumnPtr replicate(const IColumn::Offsets_t & offsets) const
	{
		size_t size = this->data.size();
		if (size != offsets.size())
			throw Exception("Size of offsets doesn't match size of column.", ErrorCodes::SIZES_OF_COLUMNS_DOESNT_MATCH);

		Self * res_ = new Self;
		ColumnPtr res = res_;
		typename Self::Container_t & res_data = res_->getData();
		res_data.reserve(offsets.back());

		IColumn::Offset_t prev_offset = 0;
		for (size_t i = 0; i < size; ++i)
		{
			size_t size_to_replicate = offsets[i] - prev_offset;
			prev_offset = offsets[i];

			for (size_t j = 0; j < size_to_replicate; ++j)
				res_data.push_back(this->data[i]);
		}

		return res;
	}

	void getExtremes(Field & min, Field & max) const
	{
		size_t size = this->data.size();

		if (size == 0)
		{
			min = typename NearestFieldType<T>::Type(0);
			max = typename NearestFieldType<T>::Type(0);
			return;
		}

		T cur_min = this->data[0];
		T cur_max = this->data[0];

		for (size_t i = 1; i < size; ++i)
		{
			if (this->data[i] < cur_min)
				cur_min = this->data[i];

			if (this->data[i] > cur_max)
				cur_max = this->data[i];
		}

		min = typename NearestFieldType<T>::Type(cur_min);
		max = typename NearestFieldType<T>::Type(cur_max);
	}
};


}
