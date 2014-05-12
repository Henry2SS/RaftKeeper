#pragma once

#include <DB/Common/HashTable/Hash.h>
#include <DB/Common/HashTable/HashTable.h>
#include <DB/Common/HashTable/HashTableAllocator.h>


template <typename Key, typename TMapped, typename Hash, typename TState = HashTableNoState>
struct HashMapCell
{
	typedef TMapped Mapped;
	typedef TState State;

	typedef std::pair<Key, Mapped> value_type;
	value_type value;

	HashMapCell() {}
	HashMapCell(const Key & key_, const State & state) : value(key_, Mapped()) {}
	HashMapCell(const value_type & value_, const State & state) : value(value_) {}

	value_type & getValue()				{ return value; }
	const value_type & getValue() const { return value; }

	static Key & getKey(value_type & value)	{ return value.first; }
	static const Key & getKey(const value_type & value) { return value.first; }

	bool keyEquals(const Key & key_) const { return value.first == key_; }
	bool keyEquals(const HashMapCell & other) const { return value.first == other.value.first; }

	void setHash(size_t hash_value) {}
	size_t getHash(const Hash & hash) const { return hash(value.first); }

	bool isZero(const State & state) const { return isZero(value.first, state); }
	static bool isZero(const Key & key, const State & state) { return ZeroTraits::check(key); }

	/// Установить значение ключа в ноль.
	void setZero() { ZeroTraits::set(value.first); }

	/// Нужно ли хранить нулевой ключ отдельно (то есть, могут ли в хэш-таблицу вставить нулевой ключ).
	static constexpr bool need_zero_value_storage = true;

	/// Является ли ячейка удалённой.
	bool isDeleted() const { return false; }

	void setMapped(const value_type & value_) { value.second = value_.second; }

	/// Сериализация, в бинарном и текстовом виде.
	void write(DB::WriteBuffer & wb) const
	{
		DB::writeBinary(value.first, wb);
		DB::writeBinary(value.second, wb);
	}

	void writeText(DB::WriteBuffer & wb) const
	{
		DB::writeDoubleQuoted(value.first, wb);
		DB::writeChar(',', wb);
		DB::writeDoubleQuoted(value.second, wb);
	}

	/// Десериализация, в бинарном и текстовом виде.
	void read(DB::ReadBuffer & rb)
	{
		DB::readBinary(value.first, rb);
		DB::readBinary(value.second, rb);
	}

	void readText(DB::ReadBuffer & rb)
	{
		DB::readDoubleQuoted(value.first, rb);
		DB::assertString(",", rb);
		DB::readDoubleQuoted(value.second, rb);
	}
};


template <typename Key, typename TMapped, typename Hash, typename TState = HashTableNoState>
struct HashMapCellWithSavedHash : public HashMapCell<Key, TMapped, Hash, TState>
{
	typedef HashMapCell<Key, TMapped, Hash, TState> Base;

	size_t saved_hash;

	HashMapCellWithSavedHash() : Base() {}
	HashMapCellWithSavedHash(const Key & key_, const typename Base::State & state) : Base(key_, state) {}
	HashMapCellWithSavedHash(const typename Base::value_type & value_, const typename Base::State & state) : Base(value_, state) {}

	bool keyEquals(const Key & key_) const { return this->value.first == key_; }
	bool keyEquals(const HashMapCellWithSavedHash & other) const { return saved_hash == other.saved_hash && this->value.first == other.value.first; }

	void setHash(size_t hash_value) { saved_hash = hash_value; }
	size_t getHash(const Hash & hash) const { return saved_hash; }
};


template
<
	typename Key,
	typename Cell,
	typename Hash = DefaultHash<Key>,
	typename Grower = HashTableGrower<>,
	typename Allocator = HashTableAllocator
>
class HashMapTable : public HashTable<Key, Cell, Hash, Grower, Allocator>
{
public:
	typedef Key key_type;
	typedef typename Cell::Mapped mapped_type;
	typedef typename Cell::value_type value_type;

	mapped_type & operator[](Key x)
	{
		typename HashMapTable::iterator it;
		bool inserted;
		this->emplace(x, it, inserted);

		if (inserted)
			new(&it->second) mapped_type();

		return it->second;
	}
};


template
<
	typename Key,
	typename Mapped,
	typename Hash = DefaultHash<Key>,
	typename Grower = HashTableGrower<>,
	typename Allocator = HashTableAllocator
>
using HashMap = HashMapTable<Key, HashMapCell<Key, Mapped, Hash>, Hash, Grower, Allocator>;


template
<
	typename Key,
	typename Mapped,
	typename Hash = DefaultHash<Key>,
	typename Grower = HashTableGrower<>,
	typename Allocator = HashTableAllocator
>
using HashMapWithSavedHash = HashMapTable<Key, HashMapCellWithSavedHash<Key, Mapped, Hash>, Hash, Grower, Allocator>;
