﻿#pragma once
#include "fsa.hpp"
#include <terark/util/enum.hpp>
#include <atomic>

// File hierarchy : cspptrie.hpp ├─> cspptrie.inl ├─> cspptrie.cpp

namespace terark {

template<size_t Align> class PatriciaMem;
class MainPatricia;

#define TERARK_friend_class_Patricia \
    friend class Patricia;   \
    friend class MainPatricia;   \
    friend class PatriciaMem<4>; \
    friend class PatriciaMem<8>

/*
 * Generated Patricia trie from matching deterministic finite automaton.
 *
 * Essentially, Patricia trie is one kind of specific radix tree which radix
 * aligns to power of 2. It may be 1, as branching in every single bit, or 8
 * which commonly interpreted as sigma of ASCII code, or very by definition.
 *
 * About Patricia trie :
 *     https://en.wikipedia.org/wiki/Radix_tree
 * About Automata theory :
 *     https://en.wikipedia.org/wiki/Automata_theory
 * About Deterministic finite automaton :
 *     https://en.wikipedia.org/wiki/Deterministic_finite_automaton
 */
class TERARK_DLL_EXPORT alignas(64) Patricia
  : public MatchingDFA, public boost::noncopyable {
public:
    TERARK_ENUM_PLAIN_INCLASS(ConcurrentLevel, byte_t,
        NoWriteReadOnly,     // 0
        SingleThreadStrict,  // 1
        SingleThreadShared,  // 2, iterator with token will keep valid
        OneWriteMultiRead,   // 3
        MultiWriteMultiRead  // 4
    );
protected:
    TERARK_ENUM_PLAIN_INCLASS(TokenState, byte_t,
        ReleaseDone,
        AcquireDone,
        AcquireIdle, // only this  thread  can set to AcquireIdle
        AcquireLock, // only other threads can set to AcquireLock
        ReleaseWait,
        DisposeWait,
        DisposeDone
    );
    struct TokenFlags {
        // state and is_head must be set simultaneously as atomic
        TokenState  state;
        byte_t      is_head;
    };
    static_assert(sizeof(TokenFlags) == 2, "sizeof(TokenFlags) == 2");
    class TERARK_DLL_EXPORT TokenBase;
    struct alignas(16) LinkType {
        TokenBase* next;
        ullong     verseq;
    };

    class TERARK_DLL_EXPORT TokenBase : protected boost::noncopyable {
        TERARK_friend_class_Patricia;
    protected:
        Patricia*     m_trie;
        void*         m_value;
        void*         m_tls; // unused for ReaderToken
        ullong        m_live_verseq;
        size_t        m_thread_id;
        struct alignas(16) { // will sync with other threads
            LinkType      m_link;
            ullong        m_min_age;
            TokenFlags    m_flags;
        };
        void enqueue(Patricia*);
        bool dequeue(Patricia*, TokenBase* delptrs[], size_t* pDelnum);
        void mt_acquire(Patricia*);
        void mt_release(Patricia*);
        void mt_update(Patricia*);
        static void del_tokens(TokenBase* ptrs[], size_t num);
        TokenBase();
        virtual ~TokenBase();
    public:
        virtual void idle();
        void release();
        void dispose(); ///< delete lazy
        bool is_valid() const {
            assert(AcquireDone == m_flags.state);
            return m_min_age < m_live_verseq;
        }

        Patricia* trie() const { return m_trie; }
        const void* value() const { return m_value; }
        template<class T>
        T value_of() const {
            assert(sizeof(T) == m_trie->m_valsize);
            assert(NULL != m_value);
            assert(size_t(m_value) % m_trie->mem_align_size() == 0);
            if (sizeof(T) == 4)
              return   aligned_load<T>(m_value);
            else
              return unaligned_load<T>(m_value);
        }
        template<class T>
        T& mutable_value_of() const {
            assert(sizeof(T) == m_trie->m_valsize);
            assert(NULL != m_value);
            assert(size_t(m_value) % m_trie->mem_align_size() == 0);
            return *reinterpret_cast<T*>(m_value);
        }
    };

public:
    class TERARK_DLL_EXPORT ReaderToken : public TokenBase {
        TERARK_friend_class_Patricia;
    protected:
        virtual ~ReaderToken();
    public:
        ReaderToken();
        void acquire(Patricia*);
        bool lookup(fstring);
    };
    using ReaderTokenPtr = std::unique_ptr<ReaderToken, DisposeAsDelete>;
    class SingleReaderToken : public TokenBase {
    public:
        explicit SingleReaderToken(Patricia* trie) {
            assert(trie->concurrent_level() <= SingleThreadStrict);
            m_trie = trie;
            m_flags.state = AcquireDone;
        }
        ~SingleReaderToken() override {
            this->m_flags.state = DisposeDone;
        }
    };

    class TERARK_DLL_EXPORT WriterToken : public TokenBase {
        TERARK_friend_class_Patricia;
    protected:
        virtual bool init_value(void* valptr, size_t valsize) noexcept;
        virtual void destroy_value(void* valptr, size_t valsize) noexcept;
        virtual ~WriterToken();
    public:
        WriterToken();
        void acquire(Patricia*);
        bool insert(fstring key, void* value);
        bool lookup(fstring);
    };
    using WriterTokenPtr = std::unique_ptr<WriterToken, DisposeAsDelete>;
    class SingleWriterToken : public WriterToken {
    public:
        ~SingleWriterToken() {
            this->m_flags.state = DisposeDone;
        }
    };
    class TERARK_DLL_EXPORT Iterator : public ReaderToken, public ADFA_LexIterator {
    protected:
        Iterator(Patricia*);
        ~Iterator();
    public:
        void dispose() final;
        virtual void token_detach_iter() = 0;
    };
    using IteratorPtr = std::unique_ptr<Iterator, DisposeAsDelete>;

    struct MemStat {
        valvec<size_t> fastbin;
        size_t used_size;
        size_t capacity;
        size_t frag_size; // = fast + huge
        size_t huge_size;
        size_t huge_cnt;
        size_t lazy_free_sum;
        size_t lazy_free_cnt;
    };
    static Patricia* create(size_t valsize,
                            size_t maxMem = 512<<10,
                            ConcurrentLevel = OneWriteMultiRead);
    MemStat mem_get_stat() const;
    virtual size_t mem_align_size() const = 0;
    virtual size_t mem_frag_size() const = 0;
    virtual void mem_get_stat(MemStat*) const = 0;

    /// @returns
    ///  true: key does not exists
    ///     token->value() == NULL : reached memory limit
    ///     token->value() != NULL : insert success,
    ///                              and value is copyed to token->value()
    ///  false: key has existed
    ///
    terark_forceinline
    bool insert(fstring key, void* value, WriterToken* token) {
        return (this->*m_insert)(key, value, token);
    }

    ConcurrentLevel concurrent_level() const { return m_writing_concurrent_level; }
    virtual bool lookup(fstring key, TokenBase* token) const = 0;
    virtual void set_readonly() = 0;
    virtual bool  is_readonly() const = 0;
    virtual WriterTokenPtr& tls_writer_token() = 0;
    virtual ReaderToken* tls_reader_token() = 0;

    WriterToken* tls_writer_token_nn() {
        return tls_writer_token_nn<WriterToken>();
    }
    /// '_nn' suffix means 'not null'
    template<class WriterTokenType>
    WriterTokenType* tls_writer_token_nn() {
        WriterTokenPtr& token = tls_writer_token();
        if (terark_likely(token.get() != NULL)) {
            assert(dynamic_cast<WriterTokenType*>(token.get()) != NULL);
        }
        else {
            token.reset(new WriterTokenType());
        }
        return static_cast<WriterTokenType*>(token.get());
    }
    template<class NewFunc>
    auto tls_writer_token_nn(NewFunc New) -> decltype(New()) {
        typedef decltype(New()) PtrType;
        WriterTokenPtr& token = tls_writer_token();
        if (terark_likely(token.get() != NULL)) {
            assert(dynamic_cast<PtrType>(token.get()) != NULL);
        }
        else {
            token.reset(New());
        }
        return static_cast<PtrType>(token.get());
    }

    Iterator* new_iter(size_t root = initial_state) const;

    struct Stat {
        size_t n_fork;
        size_t n_split;
        size_t n_mark_final;
        size_t n_add_state_move;
        size_t sum() const { return n_fork + n_split + n_mark_final + n_add_state_move; }
    };
    size_t get_valsize() const { return m_valsize; }
    virtual const Stat& trie_stat() const = 0;
    virtual const Stat& sync_stat() = 0;
    virtual size_t num_words() const = 0;
    virtual void mempool_tc_populate(size_t) = 0;
    virtual size_t get_token_qlen() const noexcept = 0;
    ~Patricia();
protected:
    Patricia();
    bool insert_readonly_throw(fstring key, void* value, WriterToken*);
    typedef bool (Patricia::*insert_func_t)(fstring, void*, WriterToken*);
    insert_func_t    m_insert;
    ConcurrentLevel  m_writing_concurrent_level;
    ConcurrentLevel  m_mempool_concurrent_level;
    bool             m_is_virtual_alloc;
    uint32_t         m_valsize;
};

terark_forceinline
bool Patricia::ReaderToken::lookup(fstring key) {
    return m_trie->lookup(key, this);
}

terark_forceinline
bool Patricia::WriterToken::insert(fstring key, void* value) {
    return m_trie->insert(key, value, this);
}

terark_forceinline
bool Patricia::WriterToken::lookup(fstring key) {
    return m_trie->lookup(key, this);
}

} // namespace terark
