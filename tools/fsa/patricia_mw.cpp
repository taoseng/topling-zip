#ifdef _MSC_VER
#define _CRT_NONSTDC_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#define _SCL_SECURE_NO_WARNINGS
#include <io.h>
#else
#include <unistd.h>
#endif

#include <terark/fsa/cspptrie.inl>
#include <terark/util/autoclose.hpp>
#include <terark/util/profiling.hpp>
#include <terark/util/linebuf.hpp>
#include <terark/util/sortable_strvec.hpp>
#include <terark/util/fstrvec.hpp>
#include <terark/util/mmap.hpp>
#include <terark/util/stat.hpp>
#include <getopt.h>
#include <fcntl.h>
#include <random>
#include <thread>
#include <terark/num_to_str.hpp>

using namespace terark;

void usage(const char* prog) {
    fprintf(stderr, R"EOS(Usage: %s Options Input-TXT-File
Options:
    -h Show this help information
    -H HugePageEnum int value(kNone = 0, kMmap = 1, kTransparent = 2)
    -A set thread affinity
    -c commit/populate thread local mempool area
    -l lock input file's mmap area(needs permission)
    -m MaxMem
    -o Output-Trie-File
    -i Condurrent write interleave
    -j Mark readonly for read
    -d Read Key from mmap
    -r Reader Thread Num
    -t Writer Thread Num, can be 0 to disable multi write
    -w Writer ConcurrentLevel
    -V Use Virtual Memory(do not use malloc/posix_memalign)
    -v Value size ratio over key size
    -z Zero Value content
    -s print stat
    -S Single thread write
    -b BenchmarkLoop : Run benchmark
    -p pause after read/mmap(MAP_POPULATE) input file
If Input-TXT-File is omitted, use stdin
)EOS", prog);
    exit(1);
}

bool inline isnewline(char c) { return ('\r' == c || '\n' == c); }

bool setAffinity = false;
size_t benchmarkLoop = 0;
intptr_t maxMem = 0;
const char* bench_input_fname = NULL;
const char* patricia_trie_fname = NULL;

#if !BOOST_OS_WINDOWS
  #include <pthread.h>
  #include <sys/mman.h>
#endif

int main(int argc, char* argv[]) {
    int cpu_num = std::thread::hardware_concurrency();
#if BOOST_OS_LINUX
    int cpu_idx = 0;
    size_t cpu_size = CPU_ALLOC_SIZE(cpu_num);
    auto thread_bind_cpu = [&]() {
        if (!setAffinity)
            return;
        auto cpu_set = (cpu_set_t*)alloca(cpu_size);
        CPU_ZERO_S(cpu_size, cpu_set);
        int idx = as_atomic(cpu_idx).fetch_add(1, std::memory_order_relaxed) % cpu_num;
        assert(CPU_COUNT_S(cpu_size, cpu_set) == 0);
        CPU_SET_S(idx, cpu_size, cpu_set);
        assert(CPU_COUNT_S(cpu_size, cpu_set) == 1);
        pthread_setaffinity_np(pthread_self(), cpu_size, cpu_set);
        CPU_CLR_S(idx, cpu_size, cpu_set);
        assert(CPU_COUNT_S(cpu_size, cpu_set) == 0);
    };
#else
    #define thread_bind_cpu() // do nothing
#endif
    int write_thread_num = cpu_num;
    int read_thread_num = 0;
    bool mark_readonly = false;
    bool direct_read_input = false;
    bool print_stat = false;
    bool concWriteInterleave = false;
    bool single_thread_write = false;
    bool zeroValue = false;
    bool useVirtualMem = false;
    bool commitMemArea = false;
    bool lockMmap = false;
    bool pauseAfterReadInput = false;
    std::string ptconfstr;
    double valueRatio = 0;
    auto conLevel = Patricia::MultiWriteMultiRead;
    for (;;) {
        int opt = getopt(argc, argv, "Ab:cdhlm:o:pt:w:r:ijsSVv:zH:");
        switch (opt) {
        case -1:
            goto GetoptDone;
        case 'A':
            setAffinity = true;
            break;
        case 'b':
            {
                char* endpos = NULL;
                benchmarkLoop = strtoul(optarg, &endpos, 10);
                if ('@' == *endpos) {
                    bench_input_fname = endpos + 1;
                }
                break;
            }
        case 'c':
            commitMemArea = true;
            break;
        case 'd':
            direct_read_input = true;
            break;
        case 'i':
            concWriteInterleave = true;
            break;
        case 'l':
            lockMmap = true;
            break;
        case 'm':
            maxMem = ParseSizeXiB(optarg);
            break;
        case 'V':
            useVirtualMem = true;
            break;
        case 'o':
            patricia_trie_fname = optarg;
            break;
        case 'p':
            pauseAfterReadInput = true;
            break;
        case 't':
            write_thread_num = atoi(optarg);
            break;
        case 'w':
            if (!enum_value(optarg, &conLevel)) {
                fprintf(stderr, "ERROR: -w %s : Invalid ConcurrentLevel\n", optarg);
                return 1;
            }
            break;
        case 'r':
            read_thread_num = std::max(0, atoi(optarg));
            break;
        case 'j':
            mark_readonly = true;
            break;
        case 's':
            print_stat = true;
            break;
        case 'S':
            single_thread_write = true;
            break;
        case 'v':
            valueRatio = atof(optarg);
            break;
        case 'z':
            zeroValue = true;
            break;
        case 'H':
            ptconfstr = fstring("?hugepage=") + optarg;
            break;
        case '?':
        case 'h':
        default:
            usage(argv[0]);
        }
    }
GetoptDone:
    terark::profiling pf;
    const char* input_fname = argv[optind];
    Auto_fclose fp;
    if (input_fname) {
        fp = fopen(input_fname, "rb");
        if (NULL == fp) {
            fprintf(stderr, "FATAL: fopen(\"%s\", \"r\") = %s\n", input_fname, strerror(errno));
            return 1;
        }
    }
    else {
        fprintf(stderr, "Reading from stdin...\n");
    }
    MmapWholeFile mmap;
    {
        struct ll_stat st;
        int err = ::ll_fstat(fileno(fp.self_or(stdin)), &st);
        if (err) {
            fprintf(stderr, "ERROR: fstat failed = %s\n", strerror(errno));
        }
        else if (S_ISREG(st.st_mode)) { // OK, get file size
            bool writable = false;
            bool populate = true;
            mmap.base = mmap_load(input_fname, &mmap.size, writable, populate);
          #if !defined(_MSC_VER)
            if (lockMmap) {
                size_t sum = 0;
                long long t0 = pf.now();
                for (size_t i = 0; i < mmap.size; i += 4*1024) {
                    sum += ((byte_t*)mmap.base)[i];
                }
                long long t1 = pf.now();
                fprintf(stderr
                    , "pre-fault  mmap: time = %8.3f sec, %8.3f GB/sec, sum = %zd\n"
                    , pf.sf(t0,t1), mmap.size/pf.nf(t0,t1), sum);
                err = mlock(mmap.base, mmap.size);
                if (err) {
                    fprintf(stderr, "WARN: mlock(%s) = %s\n", input_fname, strerror(errno));
                }
            }
          #endif
            if (0 == maxMem)
                maxMem = 2*st.st_size;
        }
    }
    if (direct_read_input && read_thread_num > 0) {
        direct_read_input = false;
        fprintf(stderr, "-d is ignored because -r %d is specified\n", read_thread_num);
    }
    intptr_t argMaxMem = useVirtualMem ? -maxMem : maxMem;
    SortableStrVec strVec;
    MainPatricia trie1(sizeof(size_t), argMaxMem, conLevel, ptconfstr);
    MainPatricia trie2(sizeof(size_t), argMaxMem, Patricia::MultiWriteMultiRead, ptconfstr);
    size_t sumkeylen = 0;
    size_t sumvaluelen = 0;
    size_t numkeys = 0;
    long long t0, t1, t2, t3, t4, t5, t6;
    long long d0, d1, d2, d3, d4, d5, dd;
    auto mmapReadStrVec = [&]() {
        strVec.m_strpool.reserve(mmap.size);
        strVec.m_index.reserve(mmap.size / 16);
        char* line = (char*)mmap.base;
        char* endp = line + mmap.size;
        while (line < endp) {
            char* next = std::find(line, endp, '\n');
            strVec.push_back(fstring(line, next - line));
            line = next + 1;
        }
        sumkeylen = strVec.str_size();
    };
    auto lineReadStrVec = [&]() {
        LineBuf line;
        while (line.getline(fp.self_or(stdin)) > 0) {
            line.chomp();
            if (line.empty()) {
                fprintf(stderr, "empty line\n");
            }
            sumkeylen += line.n;
            strVec.push_back(line);
        }
        as_atomic(sumvaluelen).fetch_add(8*strVec.size());
    };
    auto readStrVec = [&]() {
        t0 = pf.now();
        if (mmap.base) {
            if (read_thread_num > 0 ||
                    (!concWriteInterleave && !direct_read_input)) {
                mmapReadStrVec();
            }
            else {
                // do not read strVec
            }
        }
        else {
            lineReadStrVec();
        }
        numkeys = strVec.size();
        t1 = pf.now();
        if (strVec.size()) {
            fprintf(stderr
                , "read %s input: time = %8.3f sec, %8.3f MB/sec, AVG = %8.3f Bytes Per Key\n"
                , mmap.base ? "mmap" : "line"
                , pf.sf(t0,t1), sumkeylen/pf.uf(t0,t1), strVec.avg_size()
            );
        }
    };
    readStrVec();
    if (pauseAfterReadInput) {
        fprintf(stderr, "Pausing for option -p, press Enter to continue\n");
        getchar();
    }
    t0 = pf.now();
    valvec<size_t> randvec(strVec.size(), valvec_no_init());
    fstrvecll fstrVec;
	if (read_thread_num > 0) {
		for (size_t i = 0; i < randvec.size(); ++i) randvec[i] = i;
		shuffle(randvec.begin(), randvec.end(), std::mt19937_64());
		t1 = pf.now();
		fprintf(stderr
			, "generate  shuff: time = %8.3f sec, %8.3f MB/sec, QPS = %8.3f M\n"
			, pf.sf(t0,t1), randvec.used_mem_size()/pf.uf(t0,t1)
			, strVec.size() / pf.uf(t0,t1)
		);
		t0 = pf.now();
		fstrVec.reserve(strVec.size());
		fstrVec.reserve_strpool(strVec.str_size());
		for(size_t i = 0; i < strVec.size(); ++i) {
			size_t j = randvec[i];
			fstrVec.push_back(strVec[j]);
		}
		t1 = pf.now();
		fprintf(stderr
			, "fstrVec   shuff: time = %8.3f sec, %8.3f MB/sec, QPS = %8.3f M\n"
			, pf.sf(t0,t1), sumkeylen/pf.uf(t0,t1), numkeys/pf.uf(t0,t1)
		);
		t0 = pf.now(); // fstrVec memcpy
		{
			fstrVec.strpool.assign(strVec.m_strpool);
			size_t offset = 0;
			auto   offsets = fstrVec.offsets.data();
			for (size_t i = 0; i < strVec.size(); ++i) {
				offsets[i] = offset;
				offset += strVec.m_index[i].length;
			}
			fstrVec.offsets.back() = offset;
		}
		t1 = pf.now();
		fprintf(stderr
			, "fstrVec  memcpy: time = %8.3f sec, %8.3f MB/sec, QPS = %8.3f M\n"
			, pf.sf(t0,t1), sumkeylen/pf.uf(t0,t1), numkeys/pf.uf(t0,t1)
		);
		t0 = pf.now(); // fstrVec append
		{
			fstrVec.offsets.erase_all();
			fstrVec.strpool.erase_all();
			fstrVec.offsets.push_back(0);
			for (size_t i = 0; i < strVec.size(); ++i)
				fstrVec.push_back(strVec[i]);
		}
		t1 = pf.now();
		fprintf(stderr
			, "fstrVec  append: time = %8.3f sec, %8.3f MB/sec, QPS = %8.3f M\n"
			, pf.sf(t0,t1), sumkeylen/pf.uf(t0,t1), numkeys/pf.uf(t0,t1)
		);
	}
    auto patricia_find = [&](MainPatricia* pt, int tid, size_t Beg, size_t End) {
        thread_bind_cpu();
        Patricia::ReaderToken& token = *pt->tls_reader_token();
        long long tt0 = pf.now();
        if (mark_readonly) {
            token.acquire(pt);
            for (size_t i = Beg; i < End; ++i) {
                fstring s = fstrVec[i];
                if (!pt->lookup(s, &token))
                    fprintf(stderr, "pttrie not found: %.*s\n", s.ilen(), s.data());
            }
        }
        else {
            for (size_t i = Beg; i < End; ++i) {
                token.acquire(pt);
                fstring s = fstrVec[i];
                if (!pt->lookup(s, &token))
                    fprintf(stderr, "pttrie not found: %.*s\n", s.ilen(), s.data());
                token.idle();
            }
        }
        long long tt1 = pf.now();
        as_atomic(dd).fetch_add(tt1 - tt0, std::memory_order_relaxed);
        token.release();
    };
    auto patricia_lb = [&](MainPatricia* pt, int tid, size_t Beg, size_t End) {
        thread_bind_cpu();
        auto& iter = *pt->new_iter();
        long long tt0 = pf.now();
        for (size_t i = Beg; i < End; ++i) {
            fstring s = fstrVec[i];
            if (!iter.seek_lower_bound(s))
                fprintf(stderr, "pttrie lower_bound failed: %.*s\n", s.ilen(), s.data());
            else
                TERARK_VERIFY(iter.word() == s);
        }
        long long tt1 = pf.now();
        as_atomic(dd).fetch_add(tt1 - tt0, std::memory_order_relaxed);
        iter.release();
        iter.dispose();
    };
    auto exec_read = [&](MainPatricia* pt, int tnum,
      std::function<void(MainPatricia*,int,size_t,size_t)> read) {
        dd = 0;
        if (tnum < 1) {
            return;
        }
        valvec<std::thread> thrVec(tnum - 1, valvec_reserve());
        for (int i = 0; i < tnum; ++i) {
            size_t Beg = (i + 0) * fstrVec.size() / tnum;
            size_t End = (i + 1) * fstrVec.size() / tnum;
            if (i < tnum-1)
                thrVec.unchecked_emplace_back([=](){read(pt, i, Beg, End);});
            else
                read(pt, i, Beg, End);
        }
        for (auto& t : thrVec) t.join();
        dd /= std::min(tnum, cpu_num);
    };
	auto pt_write = [&](int tnum, MainPatricia* ptrie) {
        dd = 0;
		auto fins = [&](int tid) {
            thread_bind_cpu();
            if (commitMemArea) {
                ptrie->mempool_tc_populate(maxMem / tnum);
            }
			//fprintf(stderr, "thread-%03d: beg = %8zd , end = %8zd , num = %8zd\n", tid, beg, end, end - beg);
            Patricia::WriterToken& token = *ptrie->tls_writer_token_nn();
            token.acquire(ptrie);
            long long tt0 = pf.now();
            size_t sumvaluelen1 = 0;
            auto insert_v0 = [&](fstring key, size_t i) {
                if (ptrie->insert(key, &i, &token)) {
                    if (!token.has_value()) {
                        // Patricia run out of maxMem
                        fprintf(stderr
                            , "thread-%02d write concurrent run out of maxMem = %zd, i = %zd, fragments = %zd\n"
                            , tid, maxMem, i, ptrie->mem_frag_size());
                        return false;
                    }
                }
                return true;
            };
            auto insert_vx = [&](fstring key, size_t i) {
                struct PosLen { uint32_t pos, len; };
                PosLen pl{UINT32_MAX, uint32_t(valueRatio*key.size())};
                pl.pos = ptrie->mem_alloc(std::max(pl.len, 1u));
                if (uint32_t(MainPatricia::mem_alloc_fail) == pl.pos) {
                    fprintf(stderr
                        , "thread-%02d value alloc %d run out of maxMem = %zd, i = %zd, fragments = %zd\n"
                        , int(pl.len)
                        , tid, maxMem, i, ptrie->mem_frag_size());
                    return false;
                }
                char* pv = (char*)ptrie->mem_get(pl.pos);
                if (zeroValue) {
                    memset(pv, 0, pl.len);
                }
                else {
                    for(size_t j = 0; j < pl.len; ) {
                        size_t n = std::min(pl.len - j, key.size());
                        memcpy(pv + j, key.p, n);
                        j += n;
                    }
                }
                if (ptrie->insert(key, &pl, &token)) {
                    if (!token.has_value()) {
                        // Patricia run out of maxMem
                        fprintf(stderr
                            , "thread-%02d write concurrent run out of maxMem = %zd, i = %zd, fragments = %zd\n"
                            , tid, maxMem, i, ptrie->mem_frag_size());
                        return false;
                    }
                }
                sumvaluelen1 += pl.len;
                return true;
            };
            if (mmap.base && direct_read_input) {
                auto mmap_endp = (const char*)mmap.base + mmap.size;
                auto line = (const char*)mmap.base + mmap.size * (size_t(tid) + 0) / tnum;
                auto endp = (const char*)mmap.base + mmap.size * (size_t(tid) + 1) / tnum;
                //terark::minimize(endp, mmap_endp);
                assert(endp <= mmap_endp);
                while (endp < mmap_endp && !isnewline(*endp)) endp++;
                if (0 != tid) {
                    while (line < endp && !isnewline(*line)) line++;
                    while (line < endp &&  isnewline(*line)) line++;
                }
                size_t i = 0;
                size_t sumkeylen1 = 0;
                if (valueRatio > 0) {
                    while (line < endp) {
                        const char* next = line;
                        while (next < endp && !isnewline(*next)) next++;
                        if (!insert_vx(fstring(line, next), i)) {
                            break;
                        }
                        sumkeylen1 += next - line;
                        while (next < endp && isnewline(*next)) next++;
                        line = next;
                        i++;
                    }
                }
                else {
                    while (line < endp) {
                        const char* next = line;
                        while (next < endp && !isnewline(*next)) next++;
                        if (!insert_v0(fstring(line, next), i)) {
                            break;
                        }
                        sumkeylen1 += next - line;
                        while (next < endp && isnewline(*next)) next++;
                        line = next;
                        i++;
                    }
                }
                as_atomic(sumvaluelen).fetch_add(sumvaluelen1 + 8*i);
                as_atomic(sumkeylen).fetch_add(sumkeylen1);
                as_atomic(numkeys).fetch_add(i);
            }
            else {
                size_t beg = strVec.size() * (tid + 0) / tnum;
                size_t end = strVec.size() * (tid + 1) / tnum;
                for (size_t i = beg; i < end; ++i) {
                    fstring s = strVec[i];
                    if (ptrie->insert(s, &i, &token)) {
                        if (!token.has_value()) {
                            // Patricia run out of maxMem
                            fprintf(stderr
                                , "thread-%02d write concurrent run out of maxMem = %zd, i = %zd, fragments = %zd\n"
                                , tid, maxMem, i, ptrie->mem_frag_size());
                            break;
                        }
                    }
                }
            }
            long long tt1 = pf.now();
            as_atomic(dd).fetch_add(tt1 - tt0, std::memory_order_relaxed);
            token.release();
		};
		auto finsInterleave = [&](int tid) {
            thread_bind_cpu();
            if (commitMemArea) {
                ptrie->mempool_tc_populate(maxMem / tnum);
            }
			//fprintf(stderr, "thread-%03d: interleave, num = %8zd\n", tid, strVec.size() / tnum);
            Patricia::WriterToken& token = *ptrie->tls_writer_token_nn();
            token.acquire(ptrie);
            long long tt0 = pf.now();
			for (size_t i = tid, n = strVec.size(); i < n; i += tnum) {
				fstring s = strVec[i];
				if (ptrie->insert(s, &i, &token)) {
					if (!token.has_value()) {
						// Patricia run out of maxMem
						fprintf(stderr
							, "thread-%02d write concurrent run out of maxMem = %zd, i = %zd, fragments = %zd\n"
							, tid, maxMem, i, ptrie->mem_frag_size());
						break;
					}
				}
			}
            long long tt1 = pf.now();
            as_atomic(dd).fetch_add(tt1 - tt0, std::memory_order_relaxed);
            token.release();
		};
		valvec<std::thread> thrVec(tnum, valvec_reserve());
		for (int i = 0; i < tnum; ++i) {
			auto tfunc = [=]() {
				if (concWriteInterleave)
					finsInterleave(i);
				else
					fins(i);
			};
			thrVec.unchecked_emplace_back(tfunc);
		}
		for (auto& thr: thrVec) {
			thr.join();
		}
		if (mark_readonly) {
			ptrie->set_readonly();
		}
        dd /= std::min(tnum, cpu_num);
	};
	t0 = pf.now(); if (single_thread_write) { pt_write(1, &trie1); }
    d0 = dd;
	t1 = pf.now(); if (write_thread_num)    { pt_write(write_thread_num, &trie2); }
    d1 = dd;
	t2 = pf.now(); if (single_thread_write) { exec_read(&trie1, 1, patricia_find); }
    d2 = dd;
	t3 = pf.now(); if (single_thread_write) { exec_read(&trie1, 1, patricia_lb);   }
    d3 = dd;
	t4 = pf.now(); if (write_thread_num)    { exec_read(&trie2, read_thread_num, patricia_find); }
    d4 = dd;
	t5 = pf.now(); if (write_thread_num)    { exec_read(&trie2, read_thread_num, patricia_lb);   }
    d5 = dd;
    t6 = pf.now();
  if (strVec.size() == 0) {
    fprintf(stderr, "numkeys = %zd, sumkeylen = %zd, avglen = %f\n"
        , numkeys, sumkeylen, double(sumkeylen) / numkeys
    );
  }
  if (single_thread_write) {
    fprintf(stderr
        , "patricia st_Add: time = %8.3f sec, %8.3f MB/sec, QPS = %8.3f M, memory(sum = %8.3f M, key = %8.3f M, val = %8.3f M, fragments = %7zd (%.2f%%)), words = %zd, nodes = %zd, fanout = %.3f\n"
        , pf.sf(t0, t1), (sumkeylen + sumvaluelen) / pf.uf(t0, t1), numkeys / pf.uf(t0, t1)
        , trie1.mem_size() / 1e6, (trie1.mem_size() - sumvaluelen) / 1e6, sumvaluelen / 1e6
        , trie1.mem_frag_size(), 100.0*trie1.mem_frag_size()/trie1.mem_size()
        , trie1.num_words(), trie1.v_gnode_states()
        , trie1.num_words() / double(trie1.v_gnode_states() - trie1.num_words())
    );
    fprintf(stderr
        , "patricia st_Add: real = %8.3f sec, %8.3f MB/sec, QPS = %8.3f M, memory(sum = %8.3f M, key = %8.3f M, val = %8.3f M, fragments = %7zd (%.2f%%)), words = %zd, nodes = %zd, fanout = %.3f\n"
        , pf.sf(d0), (sumkeylen + sumvaluelen) / pf.uf(d0), numkeys / pf.uf(d0)
        , trie1.mem_size() / 1e6, (trie1.mem_size() - sumvaluelen) / 1e6, sumvaluelen / 1e6
        , trie1.mem_frag_size(), 100.0*trie1.mem_frag_size()/trie1.mem_size()
        , trie1.num_words(), trie1.v_gnode_states()
        , trie1.num_words() / double(trie1.v_gnode_states() - trie1.num_words())
    );
  }
  if (write_thread_num) {
    fprintf(stderr
        , "patricia mt_Add: time = %8.3f sec, %8.3f MB/sec, QPS = %8.3f M, memory(sum = %8.3f M, key = %8.3f M, val = %8.3f M, fragments = %7zd (%.2f%%)), words = %zd, nodes = %zd, fanout = %.3f, speed ratio = %.2f\n"
        , pf.sf(t1, t2), (sumkeylen + sumvaluelen) / pf.uf(t1, t2), numkeys / pf.uf(t1, t2)
        , trie2.mem_size() / 1e6, (trie2.mem_size() - sumvaluelen) / 1e6, sumvaluelen / 1e6
        , trie2.mem_frag_size(), 100.0*trie2.mem_frag_size()/trie2.mem_size()
        , trie2.num_words(), trie2.v_gnode_states()
        , trie2.num_words() / double(trie2.v_gnode_states() - trie2.num_words())
        , pf.uf(t0, t1) / pf.uf(t1, t2)
    );
    fprintf(stderr
        , "patricia mt_Add: real = %8.3f sec, %8.3f MB/sec, QPS = %8.3f M, memory(sum = %8.3f M, key = %8.3f M, val = %8.3f M, fragments = %7zd (%.2f%%)), words = %zd, nodes = %zd, fanout = %.3f, speed ratio = %.2f\n"
        , pf.sf(d1), (sumkeylen + sumvaluelen) / pf.uf(d1), numkeys / pf.uf(d1)
        , trie2.mem_size() / 1e6, (trie2.mem_size() - sumvaluelen) / 1e6, sumvaluelen / 1e6
        , trie2.mem_frag_size(), 100.0*trie2.mem_frag_size()/trie2.mem_size()
        , trie2.num_words(), trie2.v_gnode_states()
        , trie2.num_words() / double(trie2.v_gnode_states() - trie2.num_words())
        , double(d0) / d1
    );
  }
	if (read_thread_num > 0 && single_thread_write) {
		fprintf(stderr
			, "patricia s.find: time = %8.3f sec, %8.3f MB/sec, QPS = %8.3f M\n"
			, pf.sf(t2,t3), sumkeylen/pf.uf(t2,t3), numkeys/pf.uf(t2,t3)
		);
		fprintf(stderr
			, "patricia s.find: real = %8.3f sec, %8.3f MB/sec, QPS = %8.3f M\n"
			, pf.sf(d2), sumkeylen/pf.uf(d2), numkeys/pf.uf(d2)
		);
		fprintf(stderr
			, "patricia s.lowb: time = %8.3f sec, %8.3f MB/sec, QPS = %8.3f M, speed ratio = %6.3f%%(over patricia point)\n"
			, pf.sf(t3,t4), sumkeylen/pf.uf(t3,t4), numkeys/pf.uf(t3,t4)
			, 100.0*(t3-t2)/(t4-t3)
		);
		fprintf(stderr
			, "patricia s.lowb: real = %8.3f sec, %8.3f MB/sec, QPS = %8.3f M, speed ratio = %6.3f%%(over patricia point)\n"
			, pf.sf(d3), sumkeylen/pf.uf(d3), numkeys/pf.uf(d3)
			, 100.0*d2/d3
		);
	}
	if (read_thread_num > 0 && write_thread_num > 0) {
		fprintf(stderr
			, "patricia m.find: time = %8.3f sec, %8.3f MB/sec, QPS = %8.3f M\n"
			, pf.sf(t4,t5), sumkeylen/pf.uf(t4,t5), numkeys/pf.uf(t4,t5)
		);
		fprintf(stderr
			, "patricia m.find: real = %8.3f sec, %8.3f MB/sec, QPS = %8.3f M\n"
			, pf.sf(d4), sumkeylen/pf.uf(d4), numkeys/pf.uf(d4)
		);
		fprintf(stderr
			, "patricia m.lowb: time = %8.3f sec, %8.3f MB/sec, QPS = %8.3f M, speed ratio = %6.3f%%(over patricia point)\n"
			, pf.sf(t5,t6), sumkeylen/pf.uf(t5,t6), numkeys/pf.uf(t5,t6)
			, 100.0*(t5-t4)/(t6-t5)
		);
		fprintf(stderr
			, "patricia m.lowb: time = %8.3f sec, %8.3f MB/sec, QPS = %8.3f M, speed ratio = %6.3f%%(over patricia point)\n"
			, pf.sf(d5), sumkeylen/pf.uf(d5), numkeys/pf.uf(d5)
			, 100.0*d4/d5
		);
	}
    if (patricia_trie_fname) {
        t0 = pf.now();
        if (single_thread_write) {
            trie1.save_mmap(std::string(patricia_trie_fname) + ".s");
        }
        t1 = pf.now();
        if (write_thread_num) {
            trie2.save_mmap(std::string(patricia_trie_fname) + ".m");
        }
        t2 = pf.now();
        if (single_thread_write) {
          fprintf(stderr
            , "patricia s.save: time = %8.3f sec, %8.3f MB/sec, QPS = %8.3f M, mem_size = %9.3f M\n"
            , pf.sf(t0, t1), trie1.mem_size() / pf.uf(t0, t1), numkeys / pf.uf(t0, t1)
            , trie1.mem_size() / 1e6
          );
        }
        if (write_thread_num) {
          fprintf(stderr
            , "patricia m.save: time = %8.3f sec, %8.3f MB/sec, QPS = %8.3f M, mem_size = %9.3f M\n"
            , pf.sf(t1, t2), trie2.mem_size() / pf.uf(t1, t2), numkeys / pf.uf(t1, t2)
            , trie2.mem_size() / 1e6
          );
        }
    }
    if (read_thread_num > 0) {
      auto bench_iter = [&](MainPatricia* pt, char smThread) {
        t0 = pf.now();
        Patricia::IteratorPtr iter(pt->new_iter());
        bool ok = iter->seek_begin();
        for (size_t i = 0; i < strVec.size(); ++i) {
            assert(ok);
            assert(iter->word() == strVec[i]);
            ok = iter->incr();
        }
        TERARK_UNUSED_VAR(ok);
        t1 = pf.now();
        fprintf(stderr
            , "patricia %c.iter: time = %8.3f sec, %8.3f MB/sec, QPS = %8.3f M\n"
            , smThread // single/multi thread
            , pf.sf(t0, t1), sumkeylen / pf.uf(t0, t1), numkeys / pf.uf(t0, t1)
        );
        iter->release();
      };
      if (single_thread_write) { bench_iter(&trie1, 's'); }
      if (write_thread_num)    { bench_iter(&trie2, 'm'); }
    }
  auto stat_trie = [&](MainPatricia* pt) {
    auto stat = pt->trie_stat();
    double sum = stat.sum() / 100.0;
    fprintf(stderr, "fstrVec    size: %8zd\n", fstrVec.size());
    fprintf(stderr, "patricia   size: %8zd\n", pt->num_words());
    fprintf(stderr
        , "patricia   stat|             fork  |           split  |      mark_final  |  add_state_move  |\n"
          "sum = %9zd| %8zd : %5.2f%% |%8zd : %5.2f%% |%8zd : %5.2f%% |%8zd : %5.2f%% |\n"
        , stat.sum()
        , stat.n_fork, stat.n_fork/sum
        , stat.n_split, stat.n_split/sum
        , stat.n_mark_final, stat.n_mark_final/sum
        , stat.n_add_state_move, stat.n_add_state_move/sum
    );
    if (!print_stat) {
        return;
    }
    auto ms = pt->mem_get_stat();
    fprintf(stderr, "------------------------------------------------------------------------\n");
    fprintf(stderr
        , " lazyfreelist  |   mem_cnt  | %10.6f M |            |\n"
        , ms.lazy_free_cnt / 1e6
    );
    fprintf(stderr
        , " lazyfreelist  |   mem_sum  | %10.6f M | %9.2f%% |\n"
        , ms.lazy_free_sum / 1e6
        , 100.0* ms.lazy_free_sum / ms.used_size
    );
    fprintf(stderr
        , " fragments     |   mem_sum  | %10.6f M | %9.2f%% |\n"
        , pt->mem_frag_size() / 1e6
        , 100.0* pt->mem_frag_size() / ms.used_size
    );
    fprintf(stderr
        , " real used     |   mem_sum  | %10.6f M | %9.2f%% |\n"
        , (ms.used_size - ms.lazy_free_sum - pt->mem_frag_size()) / 1e6
        , (ms.used_size - ms.lazy_free_sum - pt->mem_frag_size())* 100.0 / ms.used_size
    );
    fprintf(stderr, "------------------------------------------------------------------------\n");
    fprintf(stderr, "mpool  fastbin | block size | entry num | total size | size ratio |\n");
    size_t sum_fast_cnt = 0;
    size_t sum_fast_size = 0;
    for(size_t i = 0; i < ms.fastbin.size(); ++i) {
        size_t k = ms.fastbin[i];
        sum_fast_cnt += k;
        sum_fast_size += 4*(i+1)*k;
        if (k)
            fprintf(stderr
                , "               | %10zd | %9zd | %10zd | %9.2f%% |\n"
                , 4*(i+1), k, 4*(i+1)*k, 100.0*4*(i+1)*k/ms.frag_size
            );
    }
    fprintf(stderr
        , "               | total fast | %9zd | %10zd | %9.2f%% |\n"
        , sum_fast_cnt, sum_fast_size
        , 100.0*sum_fast_size/pt->mem_frag_size()
    );
    fprintf(stderr
        , "               | total huge | %9zd | %10zd | %9.2f%% |\n"
        , ms.huge_cnt, ms.huge_size
        , 100.0*ms.huge_size/ms.frag_size
    );
    fprintf(stderr
        , "               | total frag | %9zd | %10zd | %9.2f%% |\n"
        , sum_fast_cnt + ms.huge_cnt, ms.frag_size
        , 100.0
    );
    fprintf(stderr
        , "               |   capacity |           | %10zd | %9.2f%% |\n"
        , ms.capacity, 100.0*ms.capacity/ms.used_size
    );
    assert(pt->mem_frag_size() - sum_fast_size == ms.huge_size);
    assert(pt->mem_frag_size() == ms.frag_size);
    assert(sum_fast_size == ms.frag_size - ms.huge_size);
  };
  if (single_thread_write) {
    fprintf(stderr, "Single Thread Written Trie Stats:\n");
    stat_trie(&trie1);
  }
  if (write_thread_num) {
    fprintf(stderr, "Multi Thread Written Trie Stats:\n");
    stat_trie(&trie2);
  }

  if (single_thread_write && write_thread_num > 1) {
    fprintf(stderr, "verify multi-written trie iter...\n");
    TERARK_RT_assert(trie1.num_words() == trie2.num_words(), std::logic_error);
    t0 = pf.now();
    Patricia::IteratorPtr iter1(trie1.new_iter());
    Patricia::IteratorPtr iter2(trie2.new_iter());
    fprintf(stderr, "verify multi & single thread written trie iter incr...");
    fflush(stderr);
    bool b1 = iter1->seek_begin();
    bool b2 = iter2->seek_begin();
    TERARK_RT_assert(b1 == b2, std::logic_error);
    while (b1) {
        TERARK_RT_assert(true == b2, std::logic_error);
        TERARK_RT_assert(iter1->word() == iter2->word(), std::logic_error);
        b1 = iter1->incr();
        b2 = iter2->incr();
    }
    assert(false == b2);
    t1 = pf.now();
    fprintf(stderr, " done, decr...");
    fflush(stderr);
    t2 = pf.now();
    b1 = iter1->seek_end();
    b2 = iter2->seek_end();
    TERARK_RT_assert(b1 == b2, std::logic_error);
    while (b1) {
        TERARK_RT_assert(true == b2, std::logic_error);
        TERARK_RT_assert(iter1->word() == iter2->word(), std::logic_error);
        b1 = iter1->decr();
        b2 = iter2->decr();
    }
    assert(false == b2);
    t3 = pf.now();
    fprintf(stderr, " done!\n");
    fprintf(stderr, "incr time = %f sec, throughput = %8.3f MB/sec, QPS = %8.3f M/sec\n", pf.sf(t0,t1), 2*sumkeylen/pf.uf(t0,t1), 2*numkeys/pf.uf(t0,t1));
    fprintf(stderr, "decr time = %f sec, throughput = %8.3f MB/sec, QPS = %8.3f M/sec\n", pf.sf(t2,t3), 2*sumkeylen/pf.uf(t2,t3), 2*numkeys/pf.uf(t2,t3));
    iter1->release();
    iter2->release();
  }
    return 0;
}
