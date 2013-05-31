#pragma once

#include <DB/Core/Block.h>
#include <DB/DataStreams/IBlockInputStream.h>
#include <DB/DataStreams/IBlockOutputStream.h>
#include <DB/DataStreams/IRowInputStream.h>
#include <DB/DataStreams/IRowOutputStream.h>


namespace DB
{

/** Копирует данные из InputStream в OutputStream
  * (например, из БД в консоль и т. п.)
  */
void copyData(IBlockInputStream & from, IBlockOutputStream & to);
void copyData(IRowInputStream & from, IRowOutputStream & to);

}
