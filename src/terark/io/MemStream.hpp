﻿/* vim: set tabstop=4 : */
#pragma once

#include <assert.h>
#include <string.h> // for memcpy
#include <stdarg.h>
#include <stdio.h>  // for size_t, FILE
#include <stdexcept>
#include <utility>
#include <boost/current_function.hpp>
//#include <boost/type_traits/integral_constant.hpp>
#include <boost/mpl/bool.hpp>
#include <boost/static_assert.hpp>

#include <terark/util/throw.hpp>
#include <terark/stdtypes.hpp>
#include "IOException.hpp"

namespace terark {

TERARK_DLL_EXPORT terark_no_return void throw_EndOfFile (const char* func, size_t want, size_t available);
TERARK_DLL_EXPORT terark_no_return void throw_OutOfSpace(const char* func, size_t want, size_t available);

//! MinMemIO
//! +--MemIO
//!    +--SeekableMemIO
//!       +--AutoGrownMemIO

/**
 @brief 最有效的MemIO

 只用一个指针保存当前位置，没有范围检查，只应该在完全可预测的情况下使用这个类

 @note
  -# 如果无法预测是否会越界，禁止使用该类
 */
class TERARK_DLL_EXPORT MinMemIO
{
public:
	typedef boost::mpl::false_ is_seekable; //!< 不能 seek

	explicit MinMemIO(const void* buf = 0) : m_pos((unsigned char*)buf) {}

	void set(const void* vptr) { m_pos = (unsigned char*)vptr; }
	void set(MinMemIO y) { m_pos = y.m_pos; }

	byte readByte() { return *m_pos++; }
	int  getByte() { return *m_pos++; }
	void writeByte(byte b) { *m_pos++ = b; }

	void ensureRead(void* data, size_t length) {
		memcpy(data, m_pos, length);
		m_pos += length;
	}
	void ensureWrite(const void* data, size_t length) {
		memcpy(m_pos, data, length);
		m_pos += length;
	}

	size_t read(void* data, size_t length) {
		memcpy(data, m_pos, length);
		m_pos += length;
		return length;
	}
	size_t write(const void* data, size_t length) {
		memcpy(m_pos, data, length);
		m_pos += length;
		return length;
	}

	void flush() {} // do nothing...

	byte* current() const { return m_pos; }

	//! caller can use this function to determine an offset difference
	ptrdiff_t diff(const void* start) const throw() { return m_pos - (byte*)start; }

	byte* skip(ptrdiff_t diff) throw() {
		byte* old = m_pos;
		m_pos += diff;
		return old;
	}

	byte uncheckedReadByte() { return *m_pos++; }
	void uncheckedWriteByte(byte b) { *m_pos++ = b; }

	template<class InputStream>
	void from_input(InputStream& input, size_t length) {
		input.ensureRead(m_pos, length);
		m_pos += length;
	}
	template<class OutputStream>
	void to_output(OutputStream& output, size_t length) {
		output.ensureWrite(m_pos, length);
		m_pos += length;
	}

	ptrdiff_t buf_remain_bytes() const { return INT_MAX; }

	#include "var_int_declare_read.hpp"
	#include "var_int_declare_write.hpp"

protected:
	byte* m_pos;
};

/**
 @brief Mem Stream 操作所需的最小集合

  这个类的尺寸非常小，在极端情况下效率非常高，在使用外部提供的缓冲时，这个类是最佳的选择
  这个类可以安全地浅拷贝
 */
class TERARK_DLL_EXPORT MemIO : protected MinMemIO
{
public:
	using MinMemIO::current;
	using MinMemIO::diff;
	using MinMemIO::flush;

	MemIO() { m_end = NULL; }

	// const void* is just for convenient
	MemIO(const void* buf, size_t size) { set(buf, size); }
	MemIO(const void* beg, const void* end) { set(beg, end); }

	MemIO(const std::pair<byte*, byte*>& range) {
		m_pos = range.first;
		m_end = range.second;
	}
	MemIO(const std::pair<char*, char*>& range) {
		m_pos = (byte*)range.first;
		m_end = (byte*)range.second;
	}
	MemIO(const std::pair<signed char*, signed char*>& range) {
		m_pos = (byte*)range.first;
		m_end = (byte*)range.second;
	}

	MemIO(const std::pair<const byte*, const byte*>& range) {
		m_pos = (byte*)range.first;
		m_end = (byte*)range.second;
	}
	MemIO(const std::pair<const char*, const char*>& range) {
		m_pos = (byte*)range.first;
		m_end = (byte*)range.second;
	}
	MemIO(const std::pair<const signed char*, const signed char*>& range) {
		m_pos = (byte*)range.first;
		m_end = (byte*)range.second;
	}

	// const void* is just for convenient
	void set(const void* buf, size_t size) {
		m_pos = (byte*)buf;
		m_end = (byte*)buf + size;
	}
	void set(const void* beg, const void* end) {
		m_pos = (byte*)beg;
		m_end = (byte*)end;
	}
	void set(MemIO y) { *this = y; }

	//! test pos reach end or not
	bool eof() const throw() {
		assert(m_pos <= m_end);
		return m_pos == m_end;
	}

	byte readByte();
	int  getByte() throw();
	void writeByte(byte b);

	template<class ByteArray>
	void readAll(ByteArray& ba) {
		BOOST_STATIC_ASSERT(sizeof(ba[0]) == 1);
		ptrdiff_t len = m_end - m_pos;
		ba.reserve(len);
		if (len) {
			// must be a continuous memory block
			assert(&*(ba.end()-1) - &*ba.begin() == len-1);
			memcpy(&*ba.begin(), m_pos, len);
			m_pos = m_end;
		}
	}

	void ensureRead(void* data, size_t length) ;
	void ensureWrite(const void* data, size_t length);

	size_t read(void* data, size_t length) throw();
	size_t write(const void* data, size_t length) throw();

	// rarely used methods....
	//
	size_t remain() const throw() { return m_end - m_pos; }
	byte*  end() const throw() { return m_end; }

	/**
	 @brief 向前跳过 @a diff 个字节
	 @a 可以是负数，表示向后跳跃
	 */
	byte* skip(ptrdiff_t diff) {
		assert(m_pos + diff <= m_end);
		byte* old = m_pos;
		if (terark_likely(m_pos + diff <= m_end))
			m_pos += diff;
		else {
			THROW_STD(out_of_range
				, "diff=%ld, end-pos=%ld"
				, long(diff), long(m_end-m_pos));
		}
		return old;
	}
	ptrdiff_t buf_remain_bytes() const { return m_end - m_pos; }

	template<class InputStream>
	void from_input(InputStream& input, size_t length){
		if (terark_unlikely(m_pos + length > m_end))
			throw_OutOfSpace(BOOST_CURRENT_FUNCTION, length);
		input.ensureRead(m_pos, length);
		m_pos += length;
	}
	template<class OutputStream>
	void to_output(OutputStream& output, size_t length){
		if (terark_unlikely(m_pos + length > m_end))
			throw_EndOfFile(BOOST_CURRENT_FUNCTION, length);
		output.ensureWrite(m_pos, length);
		m_pos += length;
	}

#if defined(__GLIBC__) || defined(__CYGWIN__)
	FILE* forInputFILE();
#endif

	#include "var_int_declare_read.hpp"
	#include "var_int_declare_write.hpp"

protected:
	void throw_EndOfFile(const char* func, size_t want);
	void throw_OutOfSpace(const char* func, size_t want);

protected:
	byte* m_end; // only used by set/eof
};

class TERARK_DLL_EXPORT AutoGrownMemIO;

class TERARK_DLL_EXPORT SeekableMemIO : public MemIO
{
public:
	typedef boost::mpl::true_ is_seekable; //!< 可以 seek

	SeekableMemIO() { m_pos = m_beg = m_end = 0; }
	SeekableMemIO(void* buf, size_t size) { set(buf, size); }
	SeekableMemIO(void* beg, void* end) { set(beg, end); }
	SeekableMemIO(const MemIO& x) { set(x.current(), x.end()); }

	void set(void* buf, size_t size) throw() {
		m_pos = (byte*)buf;
		m_beg = (byte*)buf;
		m_end = (byte*)buf + size;
	}
	void set(void* beg, void* end) throw() {
		m_pos = (byte*)beg;
		m_beg = (byte*)beg;
		m_end = (byte*)end;
	}

	byte*  begin()const throw() { return m_beg; }
	byte*  buf()  const throw() { return m_beg; }
	size_t size() const throw() { return m_end-m_beg; }

	const char* c_str() const {
		assert(m_pos < m_end);
		assert('\0' == *m_pos);
		return (const char*)m_beg;
	}

	size_t tell() const throw() { return m_pos-m_beg; }

	byte* skip(ptrdiff_t diff) {
		assert(m_pos + diff <= m_end);
		assert(m_pos + diff >= m_beg);
		byte* old = m_pos;
		if (terark_likely(m_pos + diff <= m_end && m_pos + diff >= m_beg))
			m_pos += diff;
		else {
			THROW_STD(out_of_range
				, "diff=%ld, pos-beg=%ld, end-pos=%ld"
				, long(diff), long(m_pos-m_beg), long(m_end-m_pos));
		}
		return old;
	}
	byte* skip_unsafe(ptrdiff_t diff) noexcept {
		TERARK_ASSERT_LE(+diff, m_end - m_pos);
		TERARK_ASSERT_LE(-diff, m_pos - m_beg);
		byte_t* old = m_pos;
		m_pos = old + diff;
		return old;
	}

	void rewind() noexcept { m_pos = m_beg; }
	void seek(ptrdiff_t newPos);
	void seek(ptrdiff_t offset, int origin);
	void seek_unsafe(ptrdiff_t newPos) noexcept {
		TERARK_ASSERT_GE(newPos, 0);
		TERARK_ASSERT_LE(newPos, m_end - m_beg);
		m_pos = m_beg + newPos;
	}

	void swap(SeekableMemIO& that) noexcept {
		std::swap(m_beg, that.m_beg);
		std::swap(m_end, that.m_end);
		std::swap(m_pos, that.m_pos);
	}

	//@{
	//! return part of (*this) as a MemIO
	std::pair<byte*, byte*> range(size_t ibeg, size_t iend) const;
	std::pair<byte*, byte*> written() const { return std::pair<byte*, byte*>(m_beg, m_pos); }
	std::pair<byte*, byte*> head() const { return written(); }
	std::pair<byte*, byte*> tail() const { return std::pair<byte*, byte*>(m_pos, m_end); }
	std::pair<byte*, byte*> whole()const { return std::pair<byte*, byte*>(m_beg, m_end); }
	//@}

protected:
	byte* m_beg;

private:
	SeekableMemIO(AutoGrownMemIO&);
	SeekableMemIO(const AutoGrownMemIO&);
};

/**
 @brief AutoGrownMemIO 可以管理自己的 buffer

 @note
  - 如果只需要 Eofmark, 使用 MemIO 就可以了
  - 如果还需要 seekable, 使用 SeekableMemIO
 */
//template<bool Use_c_malloc>
class TERARK_DLL_EXPORT AutoGrownMemIO : public SeekableMemIO
{
	DECLARE_NONE_COPYABLE_CLASS(AutoGrownMemIO);

	void growCapAndWrite(const void* data, size_t length) noexcept;
	void growCapAndWriteByte(byte b) noexcept;

	using SeekableMemIO::size; // make it usable by app code

public:
	AutoGrownMemIO() {}
	explicit AutoGrownMemIO(size_t size) noexcept;

	~AutoGrownMemIO();

	void writeByte(byte b) noexcept {
		assert(m_pos <= m_end);
		if (terark_likely(m_pos < m_end))
			*m_pos++ = b;
		else
			growCapAndWriteByte(b);
	}

	void ensureWrite(const void* data, size_t length) noexcept {
		assert(m_pos <= m_end);
		if (terark_likely(m_pos + length <= m_end)) {
			memcpy(m_pos, data, length);
			m_pos += length;
		} else
			growCapAndWrite(data, length);
	}

	size_t write(const void* data, size_t length) noexcept {
		ensureWrite(data, length);
		return length;
	}

	size_t printf(const char* format, ...) noexcept
#ifdef __GNUC__
	__attribute__ ((__format__ (__printf__, 2, 3)))
#endif
	;

	size_t vprintf(const char* format, va_list ap) noexcept
#ifdef __GNUC__
	__attribute__ ((__format__ (__printf__, 2, 0)))
#endif
	;

#if defined(__GLIBC__) || defined(__CYGWIN__)
	FILE* forFILE(const char* mode);
#endif
	void clone(const AutoGrownMemIO& src) noexcept;

	// rarely used methods....
	//
	void reserve(size_t newcap) noexcept;
	void grow_capacity(size_t nGrow) noexcept;
	void init(size_t size) noexcept;

	// synonym to size(), this is different to std::vector, user code should
	// avoid using size(), prefer tell() or capacity(),
	// because size() is somewhat ambiguous:
	//   1. In stream semantic: size() is capacity()
	//   2. In vector semantic: size() is stream.tell()
	size_t capacity() noexcept { return m_end - m_beg; }

	template<class InputStream>
	void from_input(InputStream& input, size_t length) {
		if (terark_unlikely(m_pos + length > m_end))
			reserve(tell() + length);
		input.ensureRead(m_pos, length);
		m_pos += length;
	}

	void clear() noexcept;
	void swap(AutoGrownMemIO& that) noexcept { SeekableMemIO::swap(that); }
	void shrink_to_fit() noexcept;

	void risk_take_ownership(void* buf, size_t size) noexcept {
		assert(NULL == m_beg); // must have no memory
		m_beg = m_pos = (byte*)buf;
		m_end = (byte*)buf + size;
	}

	void risk_release_ownership() noexcept {
		this->m_beg = NULL;
		this->m_end = NULL;
		this->m_pos = NULL;
	}

	byte* release() noexcept {
		byte* tmp = this->m_beg;
		this->m_beg = NULL;
		this->m_end = NULL;
		this->m_pos = NULL;
		return tmp;
	}

	template<class DataIO>
	friend
	void DataIO_loadObject(DataIO& dio, AutoGrownMemIO& x) {
		typename DataIO::my_var_size_t length;
		dio >> length;
		x.reserve(length.t);
		dio.ensureRead(x.begin(), length.t);
	}

	template<class DataIO>
	friend
	void DataIO_saveObject(DataIO& dio, const AutoGrownMemIO& x) {
		dio << typename DataIO::my_var_size_t(x.tell());
		dio.ensureWrite(x.begin(), x.tell());
	}

	#include "var_int_declare_write.hpp"

private:
	//@{
	//! disable super::set
	//!
	void set(void* buf, size_t size);
	void set(void* beg, void* end);
	//@}

	//@{
	//! disable convert-ability to MemIO
	//! this cause gcc warning: conversion to a reference to a base class will never use a type conversion operator
	//! see SeekableMemIO::SeekableMemIO(const AutoGrownMemIO&)
//	operator const SeekableMemIO&() const;
//	operator SeekableMemIO&();
	//@}
};

//////////////////////////////////////////////////////////////////////////

/**
 * @brief 读取 length 长的数据到 data
 *
 * 这个函数还是值得 inline 的，可以参考如下手工的汇编代码：
 *
 * inlined in caller, 省略了寄存器保存和恢复指令，实际情况下也有可能不用保存和恢复
 *   mov eax, m_end
 *   sub eax, m_pos
 *   mov ecx, length
 *   mov esi, m_pos
 *   mov edi, data
 *   cld
 *   cmp eax, ecx
 *   jl  Overflow
 *   rep movsb
 *   jmp End
 * Overflow:
 *   mov ecx, eax
 *   rep movsb
 * End:
 *   mov m_pos, esi
 * --------------------------------
 * sub routine in caller:
 *   push length
 *   push data
 *   push this
 *   call MemIO::read
 *   add  esp, 12 ; 如果是 stdcall, 则没有这条语句
 */
inline size_t MemIO::read(void* data, size_t length) throw()
{
	ptrdiff_t n = m_end - m_pos;
	if (terark_unlikely(n < ptrdiff_t(length))) {
		memcpy(data, m_pos, n);
	//	m_pos = m_end;
		m_pos += n;
		return n;
	} else {
		memcpy(data, m_pos, length);
		m_pos += length;
		return length;
	}
}

inline size_t MemIO::write(const void* data, size_t length) throw()
{
	ptrdiff_t n = m_end - m_pos;
	if (terark_unlikely(n < ptrdiff_t(length))) {
		memcpy(m_pos, data, n);
	//	m_pos = m_end;
		m_pos += n;
		return n;
	} else {
		memcpy(m_pos, data, length);
		m_pos += length;
		return length;
	}
}

inline void MemIO::ensureRead(void* data, size_t length)
{
	if (terark_likely(m_pos + length <= m_end)) {
		memcpy(data, m_pos, length);
		m_pos += length;
	} else
		throw_EndOfFile(BOOST_CURRENT_FUNCTION, length);
}

inline void MemIO::ensureWrite(const void* data, size_t length)
{
	if (terark_likely(m_pos + length <= m_end)) {
		memcpy(m_pos, data, length);
		m_pos += length;
	} else
		throw_OutOfSpace(BOOST_CURRENT_FUNCTION, length);
}

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4715) // not all control paths return a value
#endif

inline byte MemIO::readByte()
{
	if (terark_likely(m_pos < m_end))
		return *m_pos++;
	else {
		throw_EndOfFile(BOOST_CURRENT_FUNCTION, 1);
		return 0; // remove compiler warning
	}
}

#ifdef _MSC_VER
#pragma warning(pop)
#endif

inline void MemIO::writeByte(byte b)
{
	if (terark_likely(m_pos < m_end))
		*m_pos++ = b;
	else
		throw_OutOfSpace(BOOST_CURRENT_FUNCTION, 1);
}
inline int MemIO::getByte() throw()
{
	if (terark_likely(m_pos < m_end))
		return *m_pos++;
	else
		return -1;
}

//////////////////////////////////////////////////////////////////////////

// AutoGrownMemIO can be dumped into DataIO

//////////////////////////////////////////////////////////////////////////

} // namespace terark
