#pragma once

#include <city.h>
#include <openssl/md5.h>

#include <DB/Common/SipHash.h>
#include <DB/Common/Arena.h>
#include <DB/Common/UInt128.h>
#include <DB/Core/Defines.h>
#include <DB/Core/StringRef.h>
#include <DB/Columns/IColumn.h>
#include <DB/Common/HashTable/HashMap.h>


template <>
struct DefaultHash<StringRef> : public StringRefHash {};


namespace DB
{

typedef std::vector<size_t> Sizes;


/// Записать набор ключей фиксированной длины в UInt128, уложив их подряд (при допущении, что они помещаются).
static inline UInt128 ALWAYS_INLINE pack128(
	size_t i, size_t keys_size, const ConstColumnPlainPtrs & key_columns, const Sizes & key_sizes)
{
	union
	{
		UInt128 key;
		char bytes[16];
	};

	memset(bytes, 0, 16);
	size_t offset = 0;
	for (size_t j = 0; j < keys_size; ++j)
	{
		StringRef key_data = key_columns[j]->getDataAt(i);
		memcpy(bytes + offset, key_data.data, key_sizes[j]);
		offset += key_sizes[j];
	}

	return key;
}


/// Хэшировать набор ключей в UInt128.
static inline UInt128 ALWAYS_INLINE hash128(
	size_t i, size_t keys_size, const ConstColumnPlainPtrs & key_columns, StringRefs & keys)
{
	UInt128 key;
	SipHash hash;

	for (size_t j = 0; j < keys_size; ++j)
	{
		/// Хэшируем ключ.
		keys[j] = key_columns[j]->getDataAtWithTerminatingZero(i);
		hash.update(keys[j].data, keys[j].size);
	}

    hash.get128(key.first, key.second);

	return key;
}


/// То же самое, но без возврата ссылок на данные ключей.
static inline UInt128 ALWAYS_INLINE hash128(
	size_t i, size_t keys_size, const ConstColumnPlainPtrs & key_columns)
{
	UInt128 key;
	SipHash hash;

	for (size_t j = 0; j < keys_size; ++j)
	{
		/// Хэшируем ключ.
		StringRef key = key_columns[j]->getDataAtWithTerminatingZero(i);
		hash.update(key.data, key.size);
	}

    hash.get128(key.first, key.second);

	return key;
}


/// Скопировать ключи в пул. Потом разместить в пуле StringRef-ы на них и вернуть указатель на первый.
static inline StringRef * ALWAYS_INLINE placeKeysInPool(
	size_t i, size_t keys_size, StringRefs & keys, Arena & pool)
{
	for (size_t j = 0; j < keys_size; ++j)
	{
		char * place = pool.alloc(keys[j].size);
		memcpy(place, keys[j].data, keys[j].size);
		keys[j].data = place;
	}

	/// Размещаем в пуле StringRef-ы на только что скопированные ключи.
	char * res = pool.alloc(keys_size * sizeof(StringRef));
	memcpy(res, &keys[0], keys_size * sizeof(StringRef));

	return reinterpret_cast<StringRef *>(res);
}


/// Скопировать ключи в пул. Потом разместить в пуле StringRef-ы на них и вернуть указатель на первый.
static inline StringRef * ALWAYS_INLINE extractKeysAndPlaceInPool(
	size_t i, size_t keys_size, const ConstColumnPlainPtrs & key_columns, StringRefs & keys, Arena & pool)
{
	for (size_t j = 0; j < keys_size; ++j)
	{
		keys[j] = key_columns[j]->getDataAtWithTerminatingZero(i);
		char * place = pool.alloc(keys[j].size);
		memcpy(place, keys[j].data, keys[j].size);
		keys[j].data = place;
	}

	/// Размещаем в пуле StringRef-ы на только что скопированные ключи.
	char * res = pool.alloc(keys_size * sizeof(StringRef));
	memcpy(res, &keys[0], keys_size * sizeof(StringRef));

	return reinterpret_cast<StringRef *>(res);
}


}
