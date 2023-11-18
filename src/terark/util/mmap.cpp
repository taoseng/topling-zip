#include "mmap.hpp"
#include "autofree.hpp"
#include "throw.hpp"
#include <terark/valvec.hpp>
#include <stdio.h>
#include <string.h>
#include <stdexcept>
#include <thread>

#ifdef _MSC_VER
	#define NOMINMAX
	#define WIN32_LEAN_AND_MEAN
	#include <io.h>
	#include <Windows.h>
#else
	#include <sys/mman.h>
	#include <sys/types.h>
	#include <sys/stat.h>
	#include <unistd.h>
#endif
#include <fcntl.h>
#include <terark/fstring.hpp>

#if !defined(MAP_POPULATE)
	#define  MAP_POPULATE 0
#endif

namespace terark {

TERARK_DLL_EXPORT
void mmap_close(void* base, size_t size) {
#ifdef _MSC_VER
	::UnmapViewOfFile(base);
#else
	::munmap(base, size);
#endif
}

TERARK_DLL_EXPORT
void*
mmap_load(const char* fname, size_t* fsize, bool writable, bool populate) {
#ifdef _MSC_VER
	//LARGE_INTEGER lsize;
	HANDLE hFile = CreateFileA(fname
		, GENERIC_READ |(writable ? GENERIC_WRITE : 0)
		, FILE_SHARE_DELETE | FILE_SHARE_READ | (writable ? FILE_SHARE_WRITE : 0)
		, NULL // lpSecurityAttributes
		, writable ? OPEN_ALWAYS : OPEN_EXISTING
		, FILE_ATTRIBUTE_NORMAL
		, NULL // hTemplateFile
		);
	if (INVALID_HANDLE_VALUE == hFile) {
		DWORD err = GetLastError();
		THROW_STD(logic_error, "CreateFile(fname=%s).Err=%d(%X)"
			, fname, err, err);
	}
	TERARK_SCOPE_EXIT(CloseHandle(hFile));
	int fd = (int)(intptr_t)hFile;
#else
	int openFlags = writable ? O_RDWR : O_RDONLY;
	int fd = ::open(fname, openFlags);
	if (fd < 0) {
		int err = errno;
		THROW_STD(logic_error, "open(fname=%s, %s) = %d(%X): %s"
			, fname, writable ? "O_RDWR" : "O_RDONLY"
			, err, err, strerror(err));
	}
	TERARK_SCOPE_EXIT(::close(fd));
#endif
	return mmap_load(fd, fname, fsize, writable, populate);
}

// fd is HANDLE in windows
TERARK_DLL_EXPORT
void*
mmap_load(int fd, const char* fname, size_t* fsize, bool writable, bool populate) {
#ifdef _MSC_VER
	LARGE_INTEGER lsize;
	HANDLE hFile = (HANDLE)(intptr_t)fd;
	if (writable) {
	//	bool isNewFile = GetLastError() != ERROR_ALREADY_EXISTS;
		bool isNewFile = GetLastError() == 0;
		if (isNewFile) {
			// truncate file...
			size_t fsize2 = std::max(size_t(4*1024), *fsize);
			LONG loSize = LONG(fsize2);
			LONG hiSize = LONG(fsize2 >> 32);
			SetFilePointer(hFile, loSize, &hiSize, SEEK_SET);
			SetEndOfFile(hFile);
			SetFilePointer(hFile, 0, NULL, SEEK_SET);
		}
	}
	if (!GetFileSizeEx(hFile, &lsize)) {
		DWORD err = GetLastError();
		THROW_STD(logic_error, "GetFileSizeEx(fname=%s).Err=%d(%X)"
			, fname, err, err);
	}
	if (lsize.QuadPart > size_t(-1)) {
		THROW_STD(logic_error, "fname=%s fsize=%I64u(%I64X) too large"
			, fname, lsize.QuadPart, lsize.QuadPart);
	}
	*fsize = size_t(lsize.QuadPart);
	if (0 == *fsize) { // allow mmap empty file with readonly
		return NULL;
	}
	DWORD flProtect = writable ? PAGE_READWRITE : PAGE_READONLY;
	if (getEnvBool("mmap_load_huge_pages")) {
		flProtect |= SEC_LARGE_PAGES;
	}
	HANDLE hMmap = CreateFileMapping(hFile, NULL, flProtect, 0, 0, NULL);
	if (NULL == hMmap) {
		DWORD err = GetLastError();
		THROW_STD(runtime_error, "CreateFileMapping(fname=%s).Err=%d(0x%X)"
			, fname, err, err);
	}
	DWORD desiredAccess = (writable ? FILE_MAP_WRITE : 0) | FILE_MAP_READ;
	void* base = MapViewOfFile(hMmap, desiredAccess, 0, 0, 0);
	if (NULL == base) {
		DWORD err = GetLastError();
		::CloseHandle(hMmap);
		THROW_STD(runtime_error, "MapViewOfFile(fname=%s).Err=%d(0x%X)"
			, fname, err, err);
	}
	if (populate) {
		WIN32_MEMORY_RANGE_ENTRY vm;
		vm.VirtualAddress = base;
		vm.NumberOfBytes  = *fsize;
		PrefetchVirtualMemory(GetCurrentProcess(), 1, &vm, 0);
	}
	::CloseHandle(hMmap); // close before UnmapViewOfFile is OK
#else
	struct stat st;
	if (::fstat(fd, &st) < 0) {
		THROW_STD(logic_error, "stat(fname=%s) = %s", fname, strerror(errno));
	}
	if (0 == st.st_size) {
		if (writable) {
			st.st_size = std::max(size_t(4*1024), *fsize);
			int err = ftruncate(fd, st.st_size);
			if (err) {
				THROW_STD(logic_error, "ftruncate(fname=%s, len=%zd) = %s"
					, fname, size_t(st.st_size), strerror(errno));
			}
		}
		else { // allow mmap empty file with readonly
			return NULL;
		}
	}
	*fsize = st.st_size;
	int flProtect = (writable ? PROT_WRITE : 0) | PROT_READ;
	int flags = MAP_SHARED | (populate ? MAP_POPULATE : 0);
  #ifdef MAP_HUGETLB
	if (getEnvBool("mmap_load_huge_pages")) {
		flags |= MAP_HUGETLB;
	}
  #endif
	void* base = ::mmap(NULL, st.st_size, flProtect, flags, fd, 0);
	if (MAP_FAILED == base) {
		THROW_STD(logic_error, "mmap(fname=%s, %s SHARED, size=%lld) = %s"
			, fname, writable ? "READWRITE" : "READ"
			, (long long)st.st_size, strerror(errno));
	}
#endif
	return base;
}


TERARK_DLL_EXPORT
void* mmap_write(const char* fname, size_t* fsize, intptr_t* pfd) {
#ifdef _MSC_VER
	LARGE_INTEGER lsize;
	HANDLE hFile = CreateFileA(fname
		, GENERIC_READ |GENERIC_WRITE
		, FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE
		, NULL // lpSecurityAttributes
		, OPEN_ALWAYS
		, FILE_ATTRIBUTE_NORMAL
		, NULL // hTemplateFile
		);
	if (INVALID_HANDLE_VALUE == hFile) {
		DWORD err = GetLastError();
		THROW_STD(logic_error, "CreateFile(fname=%s).Err=%d(%X)"
			, fname, err, err);
	}
//	bool isNewFile = GetLastError() != ERROR_ALREADY_EXISTS;
	bool isNewFile = GetLastError() == 0;
	if (isNewFile) {
		// truncate file...
		size_t fsize2 = std::max(size_t(4*1024), *fsize);
		LONG loSize = LONG(fsize2);
		LONG hiSize = LONG(fsize2 >> 32);
		SetFilePointer(hFile, loSize, &hiSize, SEEK_SET);
		SetEndOfFile(hFile);
		SetFilePointer(hFile, 0, NULL, SEEK_SET);
	}
	if (!GetFileSizeEx(hFile, &lsize)) {
		DWORD err = GetLastError();
		CloseHandle(hFile);
		THROW_STD(logic_error, "GetFileSizeEx(fname=%s).Err=%d(%X)"
			, fname, err, err);
	}
	if (lsize.QuadPart > size_t(-1)) {
		CloseHandle(hFile);
		THROW_STD(logic_error, "fname=%s fsize=%I64u(%I64X) too large"
			, fname, lsize.QuadPart, lsize.QuadPart);
	}
	*fsize = size_t(lsize.QuadPart);
	DWORD flProtect = PAGE_READWRITE;
	if (getEnvBool("mmap_load_huge_pages")) {
		flProtect |= SEC_LARGE_PAGES;
	}
	HANDLE hMmap = CreateFileMapping(hFile, NULL, flProtect, 0, 0, NULL);
	if (NULL == hMmap) {
		DWORD err = GetLastError();
		CloseHandle(hFile);
		THROW_STD(runtime_error, "CreateFileMapping(fname=%s).Err=%d(0x%X)"
			, fname, err, err);
	}
	DWORD desiredAccess = FILE_MAP_WRITE;
	void* base = MapViewOfFile(hMmap, desiredAccess, 0, 0, 0);
	if (NULL == base) {
		DWORD err = GetLastError();
		::CloseHandle(hMmap);
		::CloseHandle(hFile);
		THROW_STD(runtime_error, "MapViewOfFile(fname=%s).Err=%d(0x%X)"
			, fname, err, err);
	}
	::CloseHandle(hMmap);
	*pfd = intptr_t(hFile);
#else
	int fd = ::open(fname, O_RDWR|O_CREAT, 0644);
	if (fd < 0) {
		int err = errno;
		THROW_STD(logic_error, "open(fname=%s, O_RDWR|O_CREAT, 0644) = %d(%X): %s"
			, fname, err, err, strerror(err));
	}
	struct stat st;
	if (::fstat(fd, &st) < 0) {
		close(fd);
		THROW_STD(logic_error, "stat(fname=%s) = %s", fname, strerror(errno));
	}
	//fprintf(stderr, "st.st_size = %zd\n", st.st_size);
	// st_size of new file is not zero
	// if (0 == st.st_size) //< delete the condition
	{
		st.st_size = std::max(size_t(4*1024), *fsize);
		int err = ftruncate(fd, st.st_size);
		if (err) {
			close(fd);
			THROW_STD(logic_error, "ftruncate(fname=%s, len=%zd) = %s"
				, fname, size_t(st.st_size), strerror(errno));
		}
	}
	*fsize = st.st_size;
	void* base = ::mmap(NULL, st.st_size, PROT_WRITE|PROT_READ, MAP_SHARED, fd, 0);
	if (MAP_FAILED == base) {
		::close(fd);
		THROW_STD(logic_error, "mmap(fname=%s, READ WRITE SHARED, size=%lld) = %s"
			, fname
			, (long long)st.st_size, strerror(errno));
	}
    *pfd = fd;
#endif
	return base;
}

TERARK_DLL_EXPORT
void  mmap_close(void* base, size_t size, intptr_t fd) {
#ifdef _MSC_VER
	::UnmapViewOfFile(base);
    ::CloseHandle((HANDLE)(fd));
#else
	::munmap(base, size);
    ::close(int(fd));
#endif
}

static byte_t* adjust_bondary(byte_t* ptr, byte_t* end) {
#define isnewline(c) ('\n' == c || '\r' == c)
    while (ptr < end && !isnewline(*ptr)) ++ptr;
    while (ptr < end &&  isnewline(*ptr)) ++ptr;
    return ptr;
}

TERARK_DLL_EXPORT
void parallel_for_lines(byte_t* base, size_t size, size_t num_threads,
    const function<void(size_t tid, byte_t* beg, byte_t* end)>& func)
{
    assert(num_threads > 0);
    if (0 == num_threads)
        return;
    valvec<std::thread> thrVec(num_threads, valvec_reserve());
    size_t part_len = size / num_threads;
    auto finish_ptr = (byte_t*)base + size;
    auto thread_fun = [&](size_t tid) {
        byte_t* beg = (byte_t*)base + part_len * tid;
        byte_t* end = tid+1 < num_threads ? beg + part_len : finish_ptr;
        if (0 != tid) {
            beg = adjust_bondary(beg, end);
        }
        end = adjust_bondary(end, finish_ptr);
        func(tid, beg, end);
    };
    for (size_t i = 0; i + 1 < num_threads; ++i) {
        thrVec.unchecked_emplace_back([&,i](){thread_fun(i);});
    }
    assert(thrVec.size() + 1 == num_threads);
    thread_fun(num_threads-1); // last partition
    for (auto& t : thrVec) {
        t.join();
    }
}

} // namespace terark

