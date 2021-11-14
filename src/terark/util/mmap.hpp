#pragma once

#include <stddef.h>
#include <utility>
#include "../config.hpp"
#include "../fstring.hpp"
#include "function.hpp"

namespace terark {

TERARK_DLL_EXPORT void  mmap_close(void* base, size_t size);

TERARK_DLL_EXPORT
void* mmap_load(const char* fname, size_t* size,
				bool writable = false,
				bool populate = false);

template<class String>
void* mmap_load(const String& fname, size_t* size,
				bool writable = false,
				bool populate = false) {
	return mmap_load(fname.c_str(), size, writable, populate);
}

TERARK_DLL_EXPORT
void* mmap_write(const char* fname, size_t* fsize, intptr_t* pfd);

template<class String>
void* mmap_write(const String& fname, size_t* size, intptr_t* pfd) {
	return mmap_write(fname.c_str(), size, pfd);
}

TERARK_DLL_EXPORT
void  mmap_close(void* base, size_t size, intptr_t fd);

TERARK_DLL_EXPORT
void parallel_for_lines(byte_t* base, size_t size, size_t num_threads,
    const function<void(size_t tid, byte_t* beg, byte_t* end)>& func);

class TERARK_DLL_EXPORT MmapWholeFile {
	MmapWholeFile(const MmapWholeFile&);
	MmapWholeFile& operator=(const MmapWholeFile&);

public:
	void*  base;
	size_t size;

	~MmapWholeFile() {
		if (base) {
			mmap_close(base, size);
		}
	}
	MmapWholeFile() { base = NULL; size = 0; }
	explicit MmapWholeFile(const char* fname,
						   bool writable = false,
						   bool populate = false) {
        size = 0;
		base = mmap_load(fname, &size, writable, populate);
	}
	template<class String>
	explicit MmapWholeFile(const String& fname,
						   bool writable = false,
						   bool populate = false) {
        size = 0;
		base = mmap_load(fname, &size, writable, populate);
	}

	void swap(MmapWholeFile& y) {
		std::swap(base, y.base);
		std::swap(size, y.size);
	}

	fstring memory() const {
		return fstring{(const char*)base, (ptrdiff_t)size};
	}
	fstring memory(size_t pos, size_t len) const {
		TERARK_ASSERT_LE(pos, size);
		TERARK_ASSERT_LE(pos + len, size);
		return fstring((const char*)base + pos, len); // NOLINT
	}

	void parallel_for_lines(size_t num_threads,
	                        const function<void(size_t tid, byte_t*beg,byte_t*end)>&func)
	const {
	    terark::parallel_for_lines((byte_t*)base, size, num_threads, func);
	}
};

} // namespace terark
