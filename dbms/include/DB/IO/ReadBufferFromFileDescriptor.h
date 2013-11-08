#pragma once

#include <unistd.h>
#include <errno.h>

#include <DB/Core/Exception.h>
#include <DB/Core/ErrorCodes.h>

#include <DB/IO/ReadBuffer.h>
#include <DB/IO/WriteHelpers.h>
#include <DB/IO/BufferWithOwnMemory.h>


namespace DB
{

/** Работает с готовым файловым дескриптором. Не открывает и не закрывает файл.
  */
class ReadBufferFromFileDescriptor : public BufferWithOwnMemory<ReadBuffer>
{
protected:
	int fd;
	off_t pos_in_file; /// Какому сдвигу в файле соответствует working_buffer.end().
	
	bool nextImpl()
	{
		size_t bytes_read = 0;
		while (!bytes_read)
		{
			ssize_t res = ::read(fd, internal_buffer.begin(), internal_buffer.size());
			if (!res)
				break;
			
			if (-1 == res && errno != EINTR)
				throwFromErrno("Cannot read from file " + getFileName(), ErrorCodes::CANNOT_READ_FROM_FILE_DESCRIPTOR);

			if (res > 0)
				bytes_read += res;
		}

		pos_in_file += bytes_read;

		if (bytes_read)
			working_buffer.resize(bytes_read);
		else
			return false;

		return true;
	}

	/// Имя или описание файла
	virtual std::string getFileName()
	{
		return "(fd = " + toString(fd) + ")";
	}

public:
	ReadBufferFromFileDescriptor(int fd_, size_t buf_size = DBMS_DEFAULT_BUFFER_SIZE, char * existing_memory = NULL)
		: BufferWithOwnMemory<ReadBuffer>(buf_size, existing_memory), fd(fd_), pos_in_file(0) {}

	int getFD()
	{
		return fd;
	}

	/// Если offset такой маленький, что мы не выйдем за пределы буфера, настоящий seek по файлу не делается.
	off_t seek(off_t offset, int whence = SEEK_SET)
	{
		off_t new_pos = offset;
		if (whence == SEEK_CUR)
			new_pos = pos_in_file - (working_buffer.end() - pos) + offset;
		else if (whence != SEEK_SET)
			throw Exception("ReadBufferFromFileDescriptor::seek expects SEEK_SET or SEEK_CUR as whence", ErrorCodes::ARGUMENT_OUT_OF_BOUND);

		/// Никуда не сдвинулись.
		if (new_pos + (working_buffer.end() - pos) == pos_in_file)
			return new_pos;

		if (pos != working_buffer.end() && new_pos <= pos_in_file && new_pos >= pos_in_file - static_cast<off_t>(working_buffer.size()))
		{
			/// Остались в пределах буфера.
			pos = working_buffer.begin() + (new_pos - (pos_in_file - working_buffer.size()));
			return new_pos;
		}
		else
		{
			pos = working_buffer.end();
			off_t res = lseek(fd, new_pos, SEEK_SET);
			if (-1 == res)
				throwFromErrno("Cannot seek through file " + getFileName(), ErrorCodes::CANNOT_SEEK_THROUGH_FILE);
			pos_in_file = new_pos;
			return res;
		}
	}
};

}
