#pragma once

#include <terark/valvec.hpp>
#include <terark/fstring.hpp>
#include <terark/int_vector.hpp>

namespace terark {

class TERARK_DLL_EXPORT SortableStrVec {
public:
#pragma pack(push, 4)
#if TERARK_WORD_BITS == 64
	struct OffsetLength {
		uint64_t offset : 40; //  1T
		uint64_t length : 24; // 16M, but limit to 1M
	};
  #if defined(_MSC_VER)
    struct SEntry {
        uint64_t offset : 40; //  1T
        uint64_t length : 24; // 16M, but limit to 1M
        uint32_t seq_id;
        size_t endpos() const { return offset + length; }
    };
    static const size_t MAX_STR_NUM = UINT32_MAX;
  #else
	struct SEntry {
		uint64_t offset : 40; //  1T
		uint64_t length : 20; //  1M
		uint64_t seq_id : 36; // 64G, avg 16 byte reaches offset 1T
		size_t endpos() const { return offset + length; }
	};
    static const size_t MAX_STR_NUM = (size_t(1) << 36) - 1; // 64G-1
#endif
	static const size_t MAX_STR_POOL = (size_t(1) << 40) - 1; // 1T-1
#else
	struct OffsetLength {
		uint32_t offset;
		uint32_t length;
	};
	struct SEntry {
		uint32_t offset;
		uint32_t length;
		uint32_t seq_id;
		size_t endpos() const { return offset + length; }
	};
	static const size_t MAX_STR_POOL = (size_t(1) << 31) - 1; // 2G-1
#endif
	static const size_t MAX_STR_LEN = (size_t(1) << 20) - 1; // 1M-1

#pragma pack(pop)
	valvec<byte_t> m_strpool;
	valvec<SEntry> m_index;
	size_t m_real_str_size;
	size_t sync_real_str_size();
    MemType m_strpool_mem_type;
    SortableStrVec();
    ~SortableStrVec();
    void reserve(size_t strNum, size_t maxStrPool);
    void finish() { shrink_to_fit(); }
    void shrink_to_fit();
	double avg_size() const { return double(m_strpool.size()) / m_index.size(); }
    size_t mem_cap () const { return m_index.full_mem_size() + m_strpool.full_mem_size(); }
	size_t mem_size() const { return sizeof(SEntry) * m_index.size() + m_strpool.size(); }
	size_t str_size() const { return m_strpool.size(); }
	size_t size() const { return m_index.size(); }
	fstring operator[](size_t idx) const;
	byte_t* mutable_nth_data(size_t idx) { return m_strpool.data() + m_index[idx].offset; }
	const
	byte_t* nth_data(size_t idx) const { return m_index[idx].offset + m_strpool.data(); }
    size_t  nth_size(size_t idx) const { return m_index[idx].length; }
    size_t  nth_offset(size_t idx) const { return m_index[idx].offset; }
    size_t  nth_seq_id(size_t idx) const { return m_index[idx].seq_id; }
    size_t  nth_endpos(size_t idx) const { return m_index[idx].endpos(); }
    fstring back() const { return (*this)[m_index.size()-1]; }
	void swap(SortableStrVec&);
	void push_back(fstring str);
	void pop_back();
	void back_append(fstring str);
	void back_shrink(size_t nShrink);
	void back_grow_no_init(size_t nGrow);
	void reverse_keys();
	void sort();
	void sort_by_offset();
	void sort_by_seq_id();
	void clear();
	void build_subkeys(bool speed);
	void build_subkeys(bool speed, valvec<SEntry>& subkeys);
	void compact();
	void compress_strpool(int compressLevel);
	void make_ascending_offset();
	void make_ascending_seq_id();
	size_t lower_bound_by_offset(size_t offset) const;
	size_t upper_bound_by_offset(size_t offset) const;
	size_t upper_bound_at_pos(size_t lo, size_t hi, size_t pos, byte_t ch) const;
	size_t lower_bound(fstring) const;
	size_t upper_bound(fstring) const;
	size_t find(fstring) const;
    size_t max_strlen() const;
private:
	void compress_strpool_level_1();
	void compress_strpool_level_2();
	void compress_strpool_level_3();

	template<class DataIO>
	friend void DataIO_loadObject(DataIO& dio, SortableStrVec& x) {
		uint64_t indexSize, poolSize;
		dio >> indexSize;
		dio >> poolSize;
		x.m_index.resize_no_init(size_t(indexSize));
		x.m_strpool.resize_no_init(size_t(poolSize));
		dio.ensureRead(x.m_index.data(), x.m_index.used_mem_size());
		dio.ensureRead(x.m_strpool.data(), x.m_strpool.used_mem_size());
	}
	template<class DataIO>
	friend void DataIO_saveObject(DataIO& dio, const SortableStrVec& x) {
		dio << uint64_t(x.m_index.size());
		dio << uint64_t(x.m_strpool.size());
		dio.ensureWrite(x.m_index.data(), x.m_index.used_mem_size());
		dio.ensureWrite(x.m_strpool.data(), x.m_strpool.used_mem_size());
	}

public:
    template<class Pred>
    size_t erase_if(Pred pred) {
#ifndef NDEBUG
        // requires sorted by offset
        for (size_t k = 1; k < m_index.size(); ++k) {
            assert(m_index[k-1].offset <= m_index[k].offset);
        }
#endif
        size_t offset = 0;
        size_t j = 0, k = 0, n = m_index.size();
        for (; k < n; k++) {
            fstring str = (*this)[k];
            if (!pred(str)) {
                m_index[j].length = str.size();
                m_index[j].offset = offset;
                m_index[j].seq_id = m_index[k].seq_id;
                memmove(m_strpool.data() + offset, str.data(), str.size());
                j++;
                offset += str.size();
            }
        }
        m_index.risk_set_size(j);
        m_strpool.risk_set_size(offset);
        return j;
    }
    template<class Pred2>
    size_t erase_if2(Pred2 pred2) {
#ifndef NDEBUG
        // requires sorted by offset
        for (size_t k = 1; k < m_index.size(); ++k) {
            assert(m_index[k-1].offset <= m_index[k].offset);
        }
#endif
        size_t offset = 0;
        size_t j = 0, k = 0, n = m_index.size();
        for (; k < n; k++) {
            fstring str = (*this)[k];
            if (!pred2(k, str)) {
                m_index[j].length = str.size();
                m_index[j].offset = offset;
                m_index[j].seq_id = m_index[k].seq_id;
                memmove(m_strpool.data() + offset, str.data(), str.size());
                j++;
                offset += str.size();
            }
        }
        m_index.risk_set_size(j);
        m_strpool.risk_set_size(offset);
        return j;
    }
    template<class Pred3>
    size_t erase_if3(Pred3 pred3) {
#ifndef NDEBUG
        // requires sorted by offset
        for (size_t k = 1; k < m_index.size(); ++k) {
            assert(m_index[k-1].offset <= m_index[k].offset);
        }
#endif
        size_t offset = 0;
        size_t j = 0, k = 0, n = m_index.size();
        for (; k < n; k++) {
            fstring str = (*this)[k];
            if (!pred3(j, k, str)) {
                m_index[j].length = str.size();
                m_index[j].offset = offset;
                m_index[j].seq_id = m_index[k].seq_id;
                memmove(m_strpool.data() + offset, str.data(), str.size());
                j++;
                offset += str.size();
            }
        }
        m_index.risk_set_size(j);
        m_strpool.risk_set_size(offset);
        return j;
    }
};

terark_forceinline
fstring SortableStrVec::operator[](size_t idx) const {
	assert(idx < m_index.size());
    const SEntry* index = m_index.data();
	size_t offset = index[idx].offset;
	size_t length = index[idx].length;
    assert(offset <= m_strpool.size());
    assert(offset + length <= m_strpool.size());
	return fstring(m_strpool.data() + offset, length);
}

/// Same as SortableStrVec, but without field seq_id
class TERARK_DLL_EXPORT SortThinStrVec {
public:
#if TERARK_WORD_BITS == 64
    struct SEntry {
        uint64_t offset : 44; // 1T
        uint64_t length : 20; // 1M
        size_t endpos() const { return offset + length; }
    };
    static const size_t MAX_STR_NUM  = (size_t(1) << 40) - 1; //  1T-1
	static const size_t MAX_STR_POOL = (size_t(1) << 44) - 1; // 16T-1
#else
	struct SEntry {
		uint32_t offset;
		uint32_t length;
		size_t endpos() const { return offset + length; }
	};
    static const size_t MAX_STR_NUM  = (size_t(1) << 28) - 1; // 256M-1
	static const size_t MAX_STR_POOL = (size_t(1) << 31) - 1; //   2G-1
#endif
	static const size_t MAX_STR_LEN = (size_t(1) << 20) - 1; // 1M-1
	valvec<byte_t> m_strpool;
	valvec<SEntry> m_index;
    MemType m_strpool_mem_type;
    SortThinStrVec();
    ~SortThinStrVec();
    void reserve(size_t strNum, size_t maxStrPool);
    void finish() { shrink_to_fit(); }
    void shrink_to_fit();
	double avg_size() const { return double(m_strpool.size()) / m_index.size(); }
    size_t mem_cap () const { return m_index.full_mem_size() + m_strpool.full_mem_size(); }
	size_t mem_size() const { return sizeof(SEntry) * m_index.size() + m_strpool.size(); }
	size_t str_size() const { return m_strpool.size(); }
	size_t size() const { return m_index.size(); }
	fstring operator[](size_t idx) const;
	byte_t* mutable_nth_data(size_t idx) { return m_strpool.data() + m_index[idx].offset; }
	const
	byte_t* nth_data(size_t idx) const { return m_index[idx].offset + m_strpool.data(); }
    size_t  nth_size(size_t idx) const { return m_index[idx].length; }
    size_t  nth_offset(size_t idx) const { return m_index[idx].offset; }
    size_t  nth_seq_id(size_t idx) const { return idx; }
    size_t  nth_endpos(size_t idx) const { return m_index[idx].endpos(); }
    fstring back() const { return (*this)[m_index.size()-1]; }
	void swap(SortThinStrVec&);
	void push_back(fstring str);
	void pop_back();
	void back_append(fstring str);
	void back_shrink(size_t nShrink);
	void back_grow_no_init(size_t nGrow);
	void reverse_keys();
	void sort();
	void sort(size_t valuelen); ///< except suffix valuelen
	void sort_by_offset();
	void clear();
	void build_subkeys();
	void build_subkeys(valvec<SEntry>& subkeys);
	void compact();
	void compress_strpool(int compressLevel);
	void make_ascending_offset();
	void make_ascending_seq_id();
	size_t lower_bound_by_offset(size_t offset) const;
	size_t upper_bound_by_offset(size_t offset) const;
	size_t upper_bound_at_pos(size_t lo, size_t hi, size_t pos, byte_t ch) const;
	size_t lower_bound(fstring) const;
	size_t upper_bound(fstring) const;
	size_t find(fstring) const;
    size_t max_strlen() const;
private:
	void compress_strpool_level_1();
	void compress_strpool_level_2();
	void compress_strpool_level_3();

	template<class DataIO>
	friend void DataIO_loadObject(DataIO& dio, SortThinStrVec& x) {
		uint64_t indexSize, poolSize;
		dio >> indexSize;
		dio >> poolSize;
		x.m_index.resize_no_init(size_t(indexSize));
		x.m_strpool.resize_no_init(size_t(poolSize));
		dio.ensureRead(x.m_index.data(), x.m_index.used_mem_size());
		dio.ensureRead(x.m_strpool.data(), x.m_strpool.used_mem_size());
	}
	template<class DataIO>
	friend void DataIO_saveObject(DataIO& dio, const SortThinStrVec& x) {
		dio << uint64_t(x.m_index.size());
		dio << uint64_t(x.m_strpool.size());
		dio.ensureWrite(x.m_index.data(), x.m_index.used_mem_size());
		dio.ensureWrite(x.m_strpool.data(), x.m_strpool.used_mem_size());
	}

public:
    template<class Pred>
    size_t erase_if(Pred pred) {
#ifndef NDEBUG
        // requires sorted by offset
        for (size_t k = 1; k < m_index.size(); ++k) {
            assert(m_index[k-1].offset <= m_index[k].offset);
        }
#endif
        size_t offset = 0;
        size_t j = 0, k = 0, n = m_index.size();
        for (; k < n; k++) {
            fstring str = (*this)[k];
            if (!pred(str)) {
                m_index[j].length = str.size();
                m_index[j].offset = offset;
                memmove(m_strpool.data() + offset, str.data(), str.size());
                j++;
                offset += str.size();
            }
        }
        m_index.risk_set_size(j);
        m_strpool.risk_set_size(offset);
        return j;
    }
    template<class Pred2>
    size_t erase_if2(Pred2 pred2) {
#ifndef NDEBUG
        // requires sorted by offset
        for (size_t k = 1; k < m_index.size(); ++k) {
            assert(m_index[k-1].offset <= m_index[k].offset);
        }
#endif
        size_t offset = 0;
        size_t j = 0, k = 0, n = m_index.size();
        for (; k < n; k++) {
            fstring str = (*this)[k];
            if (!pred2(k, str)) {
                m_index[j].length = str.size();
                m_index[j].offset = offset;
                memmove(m_strpool.data() + offset, str.data(), str.size());
                j++;
                offset += str.size();
            }
        }
        m_index.risk_set_size(j);
        m_strpool.risk_set_size(offset);
        return j;
    }
    template<class Pred3>
    size_t erase_if3(Pred3 pred3) {
#ifndef NDEBUG
        // requires sorted by offset
        for (size_t k = 1; k < m_index.size(); ++k) {
            assert(m_index[k-1].offset <= m_index[k].offset);
        }
#endif
        size_t offset = 0;
        size_t j = 0, k = 0, n = m_index.size();
        for (; k < n; k++) {
            fstring str = (*this)[k];
            if (!pred3(j, k, str)) {
                m_index[j].length = str.size();
                m_index[j].offset = offset;
                memmove(m_strpool.data() + offset, str.data(), str.size());
                j++;
                offset += str.size();
            }
        }
        m_index.risk_set_size(j);
        m_strpool.risk_set_size(offset);
        return j;
    }
};

terark_forceinline
fstring SortThinStrVec::operator[](size_t idx) const {
	assert(idx < m_index.size());
    const SEntry* index = m_index.data();
	size_t offset = index[idx].offset;
	size_t length = index[idx].length;
    assert(offset <= m_strpool.size());
    assert(offset + length <= m_strpool.size());
	return fstring(m_strpool.data() + offset, length);
}

class TERARK_DLL_EXPORT FixedLenStrVec {
    size_t (*m_lower_bound_fixed)(const FixedLenStrVec*, size_t, size_t, size_t, const void*);
    size_t (*m_upper_bound_fixed)(const FixedLenStrVec*, size_t, size_t, size_t, const void*);
    size_t (*m_lower_bound_prefix)(const FixedLenStrVec*, size_t, size_t, size_t, const void*);
    size_t (*m_upper_bound_prefix)(const FixedLenStrVec*, size_t, size_t, size_t, const void*);
public:
    size_t m_fixlen;
    size_t m_size;
    MemType m_strpool_mem_type;
    valvec<byte_t> m_strpool;

    explicit FixedLenStrVec(size_t fixlen = 0);
    ~FixedLenStrVec();
    void reserve(size_t strNum, size_t maxStrPool);
    void finish() { shrink_to_fit(); }
    void shrink_to_fit();
    void risk_release_ownership();

    double avg_size() const { return m_fixlen; }
    size_t mem_cap () const { return m_strpool.capacity(); }
    size_t mem_size() const { return m_strpool.size(); }
    size_t str_size() const { return m_strpool.size(); }
    size_t size() const { return m_size; }
    fstring operator[](size_t idx) const {
        assert(idx < m_size);
        assert(m_fixlen * m_size == m_strpool.size());
        size_t fixlen = m_fixlen;
        size_t offset = fixlen * idx;
        return fstring(m_strpool.data() + offset, fixlen);
    }
    const byte_t* data() const { return m_strpool.data(); }
    byte_t* data() { return m_strpool.data(); }
    byte_t* mutable_nth_data(size_t idx) { return m_strpool.data() + m_fixlen * idx; }
    const
    byte_t* nth_data(size_t idx) const { return m_strpool.data() + m_fixlen * idx; }
    size_t  nth_size(size_t /*idx*/) const { return m_fixlen; }
    size_t  nth_offset(size_t idx) const { return m_fixlen * idx; }
    size_t  nth_seq_id(size_t idx) const { return idx; }
    size_t  nth_endpos(size_t idx) const { return m_fixlen * (idx + 1); }
    fstring back() const { return (*this)[m_size-1]; }
    const byte_t* back_data() const { return m_strpool.end() - m_fixlen; }
          byte_t* back_data()       { return m_strpool.end() - m_fixlen; }
    void update_fixlen(size_t new_fixlen);
    void swap(FixedLenStrVec&);
    void push_back(fstring str);
    void pop_back();
    void reverse_keys();
    void reverse_order();
    void sort();
    void sort(size_t valuelen); ///< m_fixlen = keylen + valuelen
    static void sort_raw(void* base, size_t num, size_t fixlen);
    static void sort_raw(void* base, size_t num, size_t fixlen, size_t valuelen);
    void clear();
    void optimize_func(); // optimize (lower|upper)_bound_fixed
    size_t lower_bound_by_offset(size_t offset) const;
    size_t upper_bound_by_offset(size_t offset) const;
    size_t upper_bound_at_pos(size_t lo, size_t hi, size_t pos, byte_t ch) const;
    size_t lower_bound(fstring k) const { return lower_bound(0, m_size, k); }
    size_t upper_bound(fstring k) const { return upper_bound(0, m_size, k); }
    size_t lower_bound(size_t lo, size_t hi, fstring) const;
    size_t upper_bound(size_t lo, size_t hi, fstring) const;
    ///@{ user should ensure k len is same as m_fixlen
    size_t lower_bound_fixed(const void* k) const { return m_lower_bound_fixed(this, 0, m_size, m_fixlen, k); }
    size_t upper_bound_fixed(const void* k) const { return m_upper_bound_fixed(this, 0, m_size, m_fixlen, k); }
    size_t lower_bound_fixed(size_t lo, size_t hi, const void* k) const { return m_lower_bound_fixed(this, lo, hi, m_fixlen, k); }
    size_t upper_bound_fixed(size_t lo, size_t hi, const void* k) const { return m_upper_bound_fixed(this, lo, hi, m_fixlen, k); }
    ///@}
    size_t lower_bound_prefix(fstring k) const { return m_lower_bound_prefix(this, 0, m_size, k.n, k.p); }
    size_t upper_bound_prefix(fstring k) const { return m_upper_bound_prefix(this, 0, m_size, k.n, k.p); }
    size_t lower_bound_prefix(size_t lo, size_t hi, fstring k) const { return m_lower_bound_prefix(this, lo, hi, k.n, k.p); }
    size_t upper_bound_prefix(size_t lo, size_t hi, fstring k) const { return m_upper_bound_prefix(this, lo, hi, k.n, k.p); }
    size_t max_strlen() const { return m_fixlen; }
};

class TERARK_DLL_EXPORT SortedStrVec {
public:
    UintVecMin0    m_offsets;
    valvec<byte_t> m_strpool;
    MemType m_offsets_mem_type;
    MemType m_strpool_mem_type;

    explicit SortedStrVec();
    ~SortedStrVec();
    void reserve(size_t strNum, size_t maxStrPool);
    void finish() { shrink_to_fit(); }
    void shrink_to_fit();

    double avg_size() const { return m_strpool.size() / double(m_offsets.size()-1); }
    size_t mem_cap () const { return m_offsets.mem_size() + m_strpool.capacity(); }
    size_t mem_size() const { return m_offsets.mem_size() + m_strpool.size(); }
    size_t str_size() const { return m_strpool.size(); }
    size_t size() const {
        assert(m_offsets.size() >= 1);
        return m_offsets.size()-1;
    }
    fstring operator[](size_t idx) const {
        assert(idx + 1 < m_offsets.size());
        size_t BegEnd[2];  m_offsets.get2(idx, BegEnd);
        return fstring(m_strpool.data() + BegEnd[0], BegEnd[1] - BegEnd[0]);
    }
    byte_t* mutable_nth_data(size_t idx) { return m_strpool.data() + m_offsets[idx]; }
    const
    byte_t* nth_data(size_t idx) const { return m_strpool.data() + m_offsets[idx]; }
    size_t  nth_size(size_t idx) const {
        size_t BegEnd[2];  m_offsets.get2(idx, BegEnd);
        return BegEnd[1] - BegEnd[0];
    }
    size_t  nth_offset(size_t idx) const { return m_offsets[idx]; }
    size_t  nth_seq_id(size_t idx) const { return idx; }
    size_t  nth_endpos(size_t idx) const { return m_offsets[idx+1]; }
    fstring back() const { return (*this)[m_offsets.size()-1]; }
    void swap(SortedStrVec&);
    void push_back(fstring str);
    void pop_back();
    void back_append(fstring str);
    void back_shrink(size_t nShrink);
    void back_grow_no_init(size_t nGrow);
    void reverse_keys();
    void sort();
    void clear();
    size_t lower_bound_by_offset(size_t offset) const;
    size_t upper_bound_by_offset(size_t offset) const;
    size_t upper_bound_at_pos(size_t lo, size_t hi, size_t pos, byte_t ch) const;
    size_t lower_bound(fstring k) const { return lower_bound(0, size(), k); }
    size_t upper_bound(fstring k) const { return upper_bound(0, size(), k); }
    size_t lower_bound(size_t start, size_t end, fstring) const;
    size_t upper_bound(size_t start, size_t end, fstring) const;
    size_t max_strlen() const;
};

template<class UintXX>
class TERARK_DLL_EXPORT SortedStrVecUintTpl {
public:
    valvec<UintXX> m_offsets;
    valvec<byte_t> m_strpool;
    uint32_t       m_delim_len;
    MemType m_offsets_mem_type;
    MemType m_strpool_mem_type;

    explicit SortedStrVecUintTpl(size_t delim_len = 0);
    explicit SortedStrVecUintTpl(valvec_no_init);
    ~SortedStrVecUintTpl();
    void reserve(size_t strNum, size_t maxStrPool);
    void finish() { shrink_to_fit(); }
    void shrink_to_fit();

    double avg_size() const { return m_strpool.size() / double(m_offsets.size()-1) - m_delim_len; }
    size_t mem_cap () const { return m_offsets.full_mem_size() + m_strpool.capacity(); }
    size_t mem_size() const { return m_offsets.full_mem_size() + m_strpool.size(); }
    size_t str_size() const { return m_strpool.size(); }
    size_t size() const {
        assert(m_offsets.size() >= 1);
        return m_offsets.size()-1;
    }
    fstring operator[](size_t idx) const {
        assert(idx+1 < m_offsets.size());
        size_t Beg = m_offsets[idx+0];
        size_t End = m_offsets[idx+1];
        return fstring(m_strpool.data() + Beg, End - Beg - m_delim_len);
    }
    byte_t* mutable_nth_data(size_t idx) { return m_strpool.data() + m_offsets[idx]; }
    const
    byte_t* nth_data(size_t idx) const { return m_strpool.data() + m_offsets[idx]; }
    size_t  nth_size(size_t idx) const {
        assert(idx+1 < m_offsets.size());
        size_t Beg = m_offsets[idx+0];
        size_t End = m_offsets[idx+1];
        return End - Beg - m_delim_len;
    }
    size_t  nth_offset(size_t idx) const { return m_offsets[idx]; }
    size_t  nth_seq_id(size_t idx) const { return idx; }
    size_t  nth_endpos(size_t idx) const { return m_offsets[idx+1] - m_delim_len; }
    fstring back() const { return (*this)[m_offsets.size()-1]; }
    void swap(SortedStrVecUintTpl&);
    void push_back(fstring str);
    void pop_back();
	void back_grow_no_init(size_t nGrow);
    void reverse_keys();
    void reverse_order();
    void sort();
    void clear();
    void risk_set_data(size_t num, void* mem, size_t mem_size);
    size_t lower_bound_by_offset(size_t offset) const;
    size_t upper_bound_by_offset(size_t offset) const;
    size_t upper_bound_at_pos(size_t lo, size_t hi, size_t pos, byte_t ch) const;
    size_t lower_bound(fstring k) const { return lower_bound(0, size(), k); }
    size_t upper_bound(fstring k) const { return upper_bound(0, size(), k); }
    size_t lower_bound(size_t start, size_t end, fstring) const;
    size_t upper_bound(size_t start, size_t end, fstring) const;
    size_t max_strlen() const;
};

using VoSortedStrVec = SortedStrVec;                  // Vo : VarWidth offset
using DoSortedStrVec = SortedStrVecUintTpl<uint32_t>; // Do : DWORD    offset
using QoSortedStrVec = SortedStrVecUintTpl<uint64_t>; // Qo : QWORD    offset

} // namespace terark
