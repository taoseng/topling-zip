#include "abstract_blob_store.hpp"
#include "blob_store_file_header.hpp"
#include <terark/fsa/fsa.hpp>
#include <terark/io/FileStream.hpp>
#include <terark/util/mmap.hpp>
#include <terark/hash_strmap.hpp>
#include <terark/gold_hash_map.hpp>
#include <terark/zbs/xxhash_helper.hpp>

#if defined(_WIN32) || defined(_WIN64)
	#define WIN32_LEAN_AND_MEAN
	#define NOMINMAX
	#include <Windows.h>
#else
	#include <unistd.h> // for usleep
  extern "C" {
    extern char **environ;
  }
#endif

namespace terark {

static const uint64_t g_dicthash_seed = 0x6873614874636944ull; // echo DictHash | od -t x8

std::string GetAllEnv() {
  std::string all;
#if defined(_WIN32) || defined(_WIN64)
  all = "GetAllEnv() is todo on windows";
#else
  char** one = environ;
  while (*one) {
    all += *one;
    all.append("\n");
    one++;
  }
#endif
  return all;
}

static hash_strmap<RegisterBlobStore::Factory>& g_getFactroyMap() {
  static hash_strmap<RegisterBlobStore::Factory> map;
  return map;
}
RegisterBlobStore::RegisterFactory::RegisterFactory
(std::initializer_list<fstring> names, Factory factory)
{
  auto& map = g_getFactroyMap();
  fstring clazz = *names.begin();
  for (fstring name : names) {
    auto ib = map.insert_i(name, factory);
    TERARK_VERIFY_S(ib.second, "dup name %s for class: %s, envs:\n%s",
                    name, clazz, GetAllEnv());
  }
}


AbstractBlobStore::Dictionary::Dictionary() {
    xxhash = XXHash64(g_dicthash_seed)("");
}

AbstractBlobStore::Dictionary::Dictionary(fstring mem) : memory(mem) {
    xxhash = XXHash64(g_dicthash_seed)(mem);
}

AbstractBlobStore::Dictionary::Dictionary(fstring mem, uint64_t hash) : memory(mem), xxhash(hash) {
    TERARK_VERIFY(!isChecksumVerifyEnabled() || hash == XXHash64(g_dicthash_seed)(mem));
}

AbstractBlobStore::Dictionary::Dictionary(terark::fstring mem, uint64_t hash, bool verified_)
    : memory(mem), xxhash(hash), verified(verified_) {
}

AbstractBlobStore::Dictionary::Dictionary(size_t size, uint64_t hash) : memory(nullptr, size), xxhash(hash) {
}

AbstractBlobStore::Builder::~Builder() {}
AbstractBlobStore::Builder*
AbstractBlobStore::Builder::
createBuilder(fstring clazz, fstring outputFileName, fstring moreConfig) {
    return nullptr;
}
AbstractBlobStore::Builder*
AbstractBlobStore::Builder::getPreBuilder() const {
    return nullptr;
}

static bool RelaxedSizeCheck(ullong header, ullong mem) {
  return align_up(header, 64) == align_up(mem, 64) && header <= mem;
}

AbstractBlobStore*
AbstractBlobStore::load_from_mmap(fstring fpath, bool mmapPopulate) {
  const bool writable = false;
  MmapWholeFile fmmap(fpath, writable, mmapPopulate);
  if (fmmap.size < sizeof(FileHeaderBase)) {
    THROW_STD(invalid_argument,
      "AbstractBlobStore File: %s bad file header\n", fpath.c_str());
  }
  const auto  header = reinterpret_cast<const FileHeaderBase*>(fmmap.base);
  const auto& map = g_getFactroyMap();
  auto find = map.find(header->className);
  if (find != map.end()) {
    if (!RelaxedSizeCheck(header->fileSize, fmmap.size)) {
      THROW_STD(invalid_argument,
        "AbstractBlobStore File: %s bad file header or size: header->fileSize = %lld, mmap.size = %lld\n"
		, fpath.c_str(), (llong)header->fileSize, (llong)fmmap.size
		);
    }
    std::unique_ptr<AbstractBlobStore> store(find->second());
    store->set_fpath(fpath);
    store->init_from_memory({(const char*)fmmap.base, (ptrdiff_t)header->fileSize}, Dictionary());
    fmmap.base = nullptr;
    fmmap.size = 0;
    store->m_isMmapData = true;
    store->m_isUserMem = true;
    return store.release();
  }
  else {
    std::unique_ptr<BaseDFA> dfa(BaseDFA::load_from(fpath));
    auto ds = dynamic_cast<AbstractBlobStore*>(dfa.get());
    if (ds) {
      dfa.release();
      return ds;
    }
    else {
      THROW_STD(invalid_argument,
        "DFA File: %s is not a AbstractBlobStore\n", fpath.c_str());
    }
  }
}

AbstractBlobStore*
AbstractBlobStore::load_from_user_memory(fstring dataMem) {
	// TODO:
	return NULL;
}

AbstractBlobStore*
AbstractBlobStore::load_from_user_memory(fstring dataMem, Dictionary dict) {
  if (size_t(dataMem.n) < sizeof(FileHeaderBase)) {
    THROW_STD(invalid_argument,
      "User Memory: %p bad memory header\n", dataMem.p);
  }
  const auto  header = reinterpret_cast<const FileHeaderBase*>(dataMem.p);
  const auto& map = g_getFactroyMap();
  auto find = map.find(header->className);
  if (find != map.end()) {
    if (!RelaxedSizeCheck(header->fileSize, dataMem.n)) {
      THROW_STD(invalid_argument,
        "User Memory: %p bad memory header or size: header=%zd, data=%zd\n",
        dataMem.p, size_t(header->fileSize), dataMem.n);
    }
    dataMem.n = header->fileSize;
    std::unique_ptr<AbstractBlobStore> store(find->second());
    store->init_from_memory(dataMem, dict);
    store->m_isMmapData = false;
    store->m_isUserMem = true;
    return store.release();
  }
  else {
    std::unique_ptr<BaseDFA> dfa(BaseDFA::load_mmap_user_mem(dataMem.p, dataMem.n));
    auto ds = dynamic_cast<AbstractBlobStore*>(dfa.get());
    if (ds) {
      dfa.release();
      return ds;
    }
    else {
      THROW_STD(invalid_argument,
        "User Memory: %p is not a AbstractBlobStore\n", dataMem.p);
    }
  }
}

void AbstractBlobStore::save_mmap(fstring fpath) const {
    FileStream fp(fpath, "wb");
    save_mmap([&fp](const void* data, size_t size) {
        fp.ensureWrite(data, size);
    });
}

const char* AbstractBlobStore::name() const {
    return m_mmapBase->className;
}

void AbstractBlobStore::set_fpath(fstring fpath) {
    free(m_fpath_str);
    m_fpath_str = (char*)malloc(fpath.size() + 1);
    m_fpath_len = (uint16_t)(fpath.size());
    TERARK_VERIFY_NE(m_fpath_str, nullptr);
    memcpy(m_fpath_str, fpath.data(), fpath.size() + 1);
}

fstring AbstractBlobStore::get_fpath() const {
    return fstring(m_fpath_str, m_fpath_len);
}

AbstractBlobStore::Dictionary AbstractBlobStore::get_dict() const {
  return {};
}

fstring AbstractBlobStore::get_mmap() const {
  return {(const char *)m_mmapBase, (ptrdiff_t)m_mmapBase->fileSize};
}

AbstractBlobStore::AbstractBlobStore()
  : m_fpath_str(nullptr)
  , m_fpath_len(0)
  , m_isMmapData(false)
  , m_isUserMem(false)
  , m_isDetachMeta(false)
  , m_dictCloseType(MemoryCloseType::Clear)
  , m_checksumLevel(0)
  , m_mmapBase(nullptr) {
    m_numRecords = 0;
    m_unzipSize = 0;
}
AbstractBlobStore::~AbstractBlobStore() {
}

void AbstractBlobStore::risk_swap(AbstractBlobStore& y) {
	std::swap(m_numRecords   , y.m_numRecords   );
	std::swap(m_unzipSize    , y.m_unzipSize    );
    std::swap(m_mmap_aio     , y.m_mmap_aio     );
    std::swap(m_supportZeroCopy, y.m_supportZeroCopy);
    std::swap(m_min_prefetch_pages, y.m_min_prefetch_pages);
    std::swap(m_fpath_str    , y.m_fpath_str    );
    std::swap(m_fpath_len    , y.m_fpath_len    );
    std::swap(m_isMmapData   , y.m_isMmapData   );
    std::swap(m_isUserMem    , y.m_isUserMem    );
    std::swap(m_isDetachMeta , y.m_isDetachMeta );
	std::swap(m_dictCloseType, y.m_dictCloseType);
	std::swap(m_checksumLevel, y.m_checksumLevel);
	std::swap(m_mmapBase     , y.m_mmapBase     );
    std::swap(m_get_record_append             , y.m_get_record_append             );
    std::swap(m_get_record_append_fiber_vm_prefetch, y.m_get_record_append_fiber_vm_prefetch);
    std::swap(m_get_record_append_CacheOffsets, y.m_get_record_append_CacheOffsets);
    std::swap(m_fspread_record_append         , y.m_fspread_record_append         );
    std::swap(m_pread_record_append           , y.m_pread_record_append           );
    std::swap(m_get_zipped_size               , y.m_get_zipped_size               );
}

FunctionAdaptBuffer::FunctionAdaptBuffer(function<void(const void* data, size_t size)> f)
  : f_(std::move(f))
{}

size_t FunctionAdaptBuffer::write(const void* vbuf, size_t length) {
  f_(vbuf, length);
  return length;
}

void FunctionAdaptBuffer::flush() {
}

} // namespace terark

