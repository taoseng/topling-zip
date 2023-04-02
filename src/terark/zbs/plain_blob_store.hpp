/*
 * plain_blob_store.hpp
 *
 *  Created on: 2017-02-10
 *      Author: leipeng
 */

#pragma once

#include "abstract_blob_store.hpp"
#include <terark/int_vector.hpp>
#include <terark/util/fstrvec.hpp>

namespace terark {

class TERARK_DLL_EXPORT PlainBlobStore : public AbstractBlobStore {
    struct FileHeader; friend struct FileHeader;
    valvec<byte_t> m_content;
    UintVecMin0    m_offsets;

    template<bool FiberVmPrefetch>
    void get_record_append_imp(size_t recID, valvec<byte_t>* recData) const;
    void fspread_record_append_imp(pread_func_t fspread, void* lambda,
                                   size_t baseOffset, size_t recID,
                                   valvec<byte_t>* recData,
                                   valvec<byte_t>* buf) const;
	size_t get_zipped_size_imp(size_t recID, CacheOffsets* co) const;
public:
    void init_from_memory(fstring dataMem, Dictionary dict) override;
    void get_meta_blocks(valvec<Block>* blocks) const override;
    void get_data_blocks(valvec<Block>* blocks) const override;
    void detach_meta_blocks(const valvec<Block>& blocks) override;
    void save_mmap(function<void(const void*, size_t)> write) const override;
    using AbstractBlobStore::save_mmap;

    PlainBlobStore();
    ~PlainBlobStore();

    void take(fstrvec& vec);

    size_t mem_size() const override;
    void reorder_zip_data(ZReorderMap& newToOld,
        function<void(const void* data, size_t size)> writeAppend,
        fstring tmpFile) const override;

    struct TERARK_DLL_EXPORT MyBuilder : public AbstractBlobStore::Builder {
        class TERARK_DLL_EXPORT Impl; Impl* impl;
    public:
        MyBuilder(size_t contentSize, size_t contentCnt, fstring fpath, size_t offset = 0,
                  int checksumLevel = 3, int checksumType = 0);
        virtual ~MyBuilder();
        void addRecord(fstring rec) override;
        void finish() override;
    };
};

} // namespace terark
