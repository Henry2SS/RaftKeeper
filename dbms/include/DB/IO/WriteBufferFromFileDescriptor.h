#pragma once

#include <unistd.h>
#include <errno.h>

#include <DB/Core/Exception.h>
#include <DB/Core/ErrorCodes.h>

#include <DB/IO/WriteBuffer.h>
#include <DB/IO/WriteHelpers.h>
#include <DB/IO/BufferWithOwnMemory.h>


namespace DB
{

/** Работает с готовым файловым дескриптором. Не открывает и не закрывает файл.
  */
class WriteBufferFromFileDescriptor : public BufferWithOwnMemory<WriteBuffer>
{
protected:
	int fd;
	
	void nextImpl()
	{
		if (!offset())
			return;

		size_t bytes_written = 0;
		while (bytes_written != offset())
		{
			ssize_t res = ::write(fd, working_buffer.begin() + bytes_written, offset() - bytes_written);

			if ((-1 == res || 0 == res) && errno != EINTR)
				throwFromErrno("Cannot write to file " + getFileName(), ErrorCodes::CANNOT_WRITE_TO_FILE_DESCRIPTOR);

			if (res > 0)
				bytes_written += res;
		}
	}

	/// Имя или описание файла
	virtual std::string getFileName()
	{
		return "(fd = " + toString(fd) + ")";
	}

public:
	WriteBufferFromFileDescriptor(int fd_ = -1, size_t buf_size = DBMS_DEFAULT_BUFFER_SIZE, char * existing_memory = NULL, size_t alignment = 0)
		: BufferWithOwnMemory<WriteBuffer>(buf_size, existing_memory, alignment), fd(fd_) {}

	/** Можно вызывать для инициализации, если нужный fd не был передан в конструктор.
	  * Менять fd во время работы нельзя.
	  */
	void setFD(int fd_)
	{
		fd = fd_;
	}

    ~WriteBufferFromFileDescriptor()
	{
		try
		{
			next();
		}
		catch (...)
		{
			tryLogCurrentException(__PRETTY_FUNCTION__);
		}
	}

	int getFD()
	{
		return fd;
	}

	off_t seek(off_t offset, int whence = SEEK_SET)
	{
		off_t res = lseek(fd, offset, whence);
		if (-1 == res)
			throwFromErrno("Cannot seek through file " + getFileName(), ErrorCodes::CANNOT_SEEK_THROUGH_FILE);
		return res;
	}

	void truncate(off_t length = 0)
	{
		int res = ftruncate(fd, length);
		if (-1 == res)
			throwFromErrno("Cannot truncate file " + getFileName(), ErrorCodes::CANNOT_TRUNCATE_FILE);
	}

	void sync()
	{
		/// Если в буфере ещё остались данные - запишем их.
		next();

		/// Попросим ОС сбросить данные на диск.
		int res = fsync(fd);
		if (-1 == res)
			throwFromErrno("Cannot fsync " + getFileName(), ErrorCodes::CANNOT_FSYNC);
	}
};

}
