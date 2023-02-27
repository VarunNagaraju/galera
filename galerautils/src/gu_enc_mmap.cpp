/* Copyright (c) 2022 Percona LLC and/or its affiliates. All rights
   reserved.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; version 2 of
   the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */

#include "gu_enc_mmap.hpp"
#include "gu_throw.hpp"
#include "gu_logger.hpp"
#include "gu_enc_debug.hpp"
#include "gu_enc_utils.hpp"
#include "gu_enc_pmemory_manager.hpp"
#include "gu_enc_pmemory_manager_pool.hpp"

#include <sys/stat.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/mman.h>

#include <cassert>
#include <thread>
#include <set>
#include <utility>


namespace gu {

#define REAL_ENCRYPTION 1

// maximum number of PMemoryManagers waiting in the pool
static const size_t MANAGERS_POOL_SIZE = 10;
PMemoryManagerPool memory_manager_pool(MANAGERS_POOL_SIZE);

// EncMMap objects repository
class EncMMapsRepository {
public:
    static void AddEncMMap(EncMMap *mmap, unsigned char* ptr, size_t size);
    static void DelEncMMap(EncMMap *mmap);
    static bool TryGetEncMMap(unsigned char* ptr, EncMMap** mmap);

    static void DumpMappings();

    EncMMapsRepository() = delete;
    EncMMapsRepository(const EncMMapsRepository&) = delete;
    EncMMapsRepository& operator=(const EncMMapsRepository&) = delete;

private:
    struct EncMMapDescriptor {
        EncMMapDescriptor(unsigned char* start, unsigned char* end)
        : start_(start)
        , end_ (end)
        {}

        unsigned char*   start_;
        unsigned char*   end_;
    };

    static std::atomic_flag enc_mmaps_lock;
    static std::map<EncMMap*, EncMMapDescriptor> enc_mmaps;
};

std::atomic_flag EncMMapsRepository::enc_mmaps_lock = ATOMIC_FLAG_INIT;
std::map<EncMMap*, EncMMapsRepository::EncMMapDescriptor> EncMMapsRepository::enc_mmaps;

void EncMMapsRepository::AddEncMMap(EncMMap *mmap, unsigned char* ptr, size_t size) {
    while (enc_mmaps_lock.test_and_set(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    enc_mmaps.emplace(std::piecewise_construct, std::forward_as_tuple(mmap),
      std::forward_as_tuple(ptr, ptr+size));
    enc_mmaps_lock.clear(std::memory_order_release);
}

void EncMMapsRepository::DelEncMMap(EncMMap *mmap) {
    while (enc_mmaps_lock.test_and_set(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    enc_mmaps.erase(mmap);
    enc_mmaps_lock.clear(std::memory_order_release);
}

bool EncMMapsRepository::TryGetEncMMap(unsigned char* ptr, EncMMap** mmap) {
    // If someone is accessing enc_mmaps, just bail out without even trying
    // to find requested object.
    // It can happen in the following situations:
    // 1. (rare) The client in registering/deregistering new EncMMap object
    // 2. (more probable but still not so often) Two signal handlers are called
    //    simultaneously (2 threads). In such a case the 2nd one will retry
    //    in a while.
    *mmap = nullptr;
    if (enc_mmaps_lock.test_and_set(std::memory_order_acquire)) {
        return true;
    }

    for (auto m : enc_mmaps) {
        if (ptr >= m.second.start_  &&  ptr < m.second.end_) {
            *mmap = m.first;
            break;
        }
    }

    enc_mmaps_lock.clear(std::memory_order_release);
    return false;
}

// Debug purpose only
void EncMMapsRepository::DumpMappings() {
    while (enc_mmaps_lock.test_and_set(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    for (auto mm : enc_mmaps) {
        S_DEBUG_A("Mappings for EncMMap x%llX (x%llX - x%llX) START\n",
            ptr2ull(mm.first), ptr2ull(mm.second.start_), ptr2ull(mm.second.end_));
        mm.first->dump_mappings();
    }

    enc_mmaps_lock.clear(std::memory_order_release);
}

void dump_mappings() {
    EncMMapsRepository::DumpMappings();
}

const char* page_protection_to_string(int page_protection){
    static std::map<int, std::string> prot2string = {
        {PROT_NONE, "PROT_NONE"},
        {PROT_READ, "PROT_READ"},
        {PROT_WRITE, "PROT_WRITE"},
        {PROT_READ | PROT_WRITE, "PROT_READ | PROT_WRITE"},
        {-1, "UNKNOWN"}
    };

    auto item = prot2string.find(page_protection);
    if (item == prot2string.end()) {
        return prot2string[-1].c_str();
    }
    return item->second.c_str();
}

void EncMMap::dump_mappings()
{
    S_DEBUG_N("vpage -> ppage mappings start\n");
    for (auto kv : vpage2ppage_) {
        S_DEBUG_A("vpage: x%llX, ppage: x%llX\n", ptr2ull(kv.first), ptr2ull(kv.second->ptr_));
    }
    S_DEBUG_N("vpage -> ppage mappings end\n");
}


// Helper methods
unsigned char* EncMMap::page_start(unsigned long long page_no) const {
    return base_ + page_size_ * page_no;
}

unsigned char* EncMMap::page_start(unsigned char* addr) const {
    unsigned long long addr_u = ptr2ull(addr);
    unsigned long long page_start = (addr_u / page_size_) * page_size_;
    return reinterpret_cast<unsigned char*>(page_start);
}

size_t EncMMap::page_number(unsigned char* addr) const {
    unsigned long long offset = ptr2ull(addr) - ptr2ull(base_);
    size_t page_no = offset / page_size_;
    return page_no;
}


// encrption / decryption
void EncMMap::encrypt(unsigned char* dst, unsigned char* src, size_t size, size_t page_number) const {
    // the last page may be not full
    size = is_last_page(page_number) ? last_page_size_ : size;
#if REAL_ENCRYPTION
    size_t page_start_offset = page_number * page_size_;
    size_t unencrypted_size = 0;

    if (gu_unlikely(page_start_offset < encryption_start_offset_)) {
        unencrypted_size = std::min(size, encryption_start_offset_);
        memcpy(dst, src, unencrypted_size);
        dst += unencrypted_size;
        src += unencrypted_size;
    }

    int encryptedSize = size - unencrypted_size;
    if(encryptedSize > 0) {
      encryptor_.set_stream_offset(page_start_offset + unencrypted_size);
      encryptor_.encrypt(dst, src, size - unencrypted_size);
    }
#else
    memcpy(dst, src, size);
#endif
}

void EncMMap::decrypt(unsigned char* dst, unsigned char* src, size_t size, size_t page_number) const {
    // the last page may be not full
    size = is_last_page(page_number) ? last_page_size_ : size;
#if REAL_ENCRYPTION
    size_t page_start_offset = page_number * page_size_;
    size_t unencrypted_size = 0;

    if (gu_unlikely(page_start_offset < encryption_start_offset_)) {
        unencrypted_size = std::min(size, encryption_start_offset_);
        memcpy(dst, src, unencrypted_size);
        dst += unencrypted_size;
        src += unencrypted_size;
    }

    int encryptedSize = size - unencrypted_size;
    if(encryptedSize > 0) {
        decryptor_.set_stream_offset(page_start_offset + unencrypted_size);
        decryptor_.decrypt(dst, src, size - unencrypted_size);
    }
#else
    memcpy(dst, src, size);
#endif
}


// signal handler install and dispatch
static std::once_flag signal_handler_once;
static struct sigaction oldsigact;

void signal_handler(int sig, siginfo_t* info, void* ctx) {
    unsigned char *addr = static_cast<unsigned char*>(info->si_addr);
    S_DEBUG_N("addr: x%llX\n", ptr2ull(addr));

    EncMMap* encmmap;
    if (EncMMapsRepository::TryGetEncMMap(addr, &encmmap)) {
        S_DEBUG_N("signal_handler collision\n");
        return;
    }

    if (encmmap == nullptr) {
        S_DEBUG_W("calling old signal handler\n");
        if (oldsigact.sa_flags == SA_SIGINFO) {
            oldsigact.sa_sigaction(sig, info, ctx);
        } else {
            oldsigact.sa_handler(sig);
        }
        return;
    }

    // This is our region. Dispatch to the proper EncMMap object.
    encmmap->handle_signal(info);
}

static void install_signal_handler() {
	struct sigaction sa;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_SIGINFO | SA_NODEFER;

	sa.sa_sigaction = &signal_handler;
	if (sigaction(SIGSEGV, &sa, &oldsigact) == -1) {
        gu_throw_error(errno) << "install_signal_handler() signal handler installation failed";
    }
}


EncMMap::EncMMap(const std::string& key, std::shared_ptr<MMap> rawmmap,
                 size_t cache_page_size, size_t cache_size, bool sync_on_destroy,
                 size_t encryption_start_offset)
: mmapraw_(rawmmap)
, page_size_(cache_page_size)
, mmapraw_ptr_(static_cast<unsigned char*>(mmapraw_->get_ptr()))
, vmem_size_(mmapraw_->get_size())
// mmap 2 pages more: 1st for aligning start, 2nd if the last underlying page is not aligned
, mmap_ptr_(static_cast<unsigned char*>(mmap(nullptr, vmem_size_ + 2*page_size_, PROT_NONE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0)))
, base_(nullptr)
, memory_manager_ptr_(memory_manager_pool.allocate(cache_page_size, cache_size))
, memory_manager_(*memory_manager_ptr_)
, vpage2protection_guard_()
, vpage2protection_(nullptr)
, vpage2ppage_()
, pages_cnt_(0)
, mapped_(mmap_ptr_ != MAP_FAILED)
, last_page_size_(page_size_)
, encryption_start_offset_(encryption_start_offset)
, default_page_protection_(PROT_READ | PROT_WRITE )
, read_ahead_cnt_(0)
, encryptor_()
, decryptor_()
, sync_on_destroy_(sync_on_destroy) {

    if (!mapped_)
    {
        gu_throw_error(errno) << "EncMMap::EncMMap() mmap() on anonymous failed";
    }

    // we need base_ to be aligned with page_size_ for easier calculations later
    // here we will loose at most page_size_ at the beginning
    base_ = reinterpret_cast<unsigned char*>(((ptr2ull(mmap_ptr_) + page_size_) / page_size_) * page_size_);

    S_DEBUG_A("EncMMap::EncMMap() this: x%llX, mmap_ptr: x%llX aligned mapping: (x%llX - x%llX) (%ld bytes)\n",
        ptr2ull(this), ptr2ull(mmap_ptr_), ptr2ull(base_), ptr2ull(base_) + vmem_size_, vmem_size_);
    // install signal handler common for all objects
    std::call_once(signal_handler_once, install_signal_handler);
    pages_cnt_ = vmem_size_ / page_size_;
    if (vmem_size_ % page_size_) {
        // if the size is not aligned, the last page is smaller than page_size_
        last_page_size_ = vmem_size_ % page_size_;
        S_DEBUG_A("EncMMap::EncMMap() adding page, size not aligned: %ld, last_page_size: %ld\n",
          vmem_size_, last_page_size_);
        pages_cnt_++;
    }

    S_DEBUG_A("EncMMap::EncMMap() allocated pages cnt: %ld\n", pages_cnt_);
    vpage2protection_guard_ = std::shared_ptr<int>(new int[pages_cnt_], [](int *p) { delete[] p; });
    vpage2protection_ = vpage2protection_guard_.get();
    EncMMapsRepository::AddEncMMap(this, base_, vmem_size_);

    // we set up vpage2protection_ map inside
    set_key(key);
}

EncMMap::~EncMMap() {
    S_DEBUG_A("EncMMap::~EncMMap() this: x%llX, mmap_ptr: x%llX aligned mapping: (x%llX - x%llX) (%ld bytes)\n",
        ptr2ull(this), ptr2ull(mmap_ptr_), ptr2ull(base_), ptr2ull(base_) + vmem_size_, vmem_size_);
    if (mapped_)
    {
        try { unmap(); } catch (Exception& e) { log_error << e.what(); }
    }

    encryptor_.close();
    decryptor_.close();

    memory_manager_.free_all();
    memory_manager_pool.free(memory_manager_ptr_);
}

bool EncMMap::try_lock() const {
    return !lock_.test_and_set(std::memory_order_acquire);
}

void EncMMap::lock() const {
    while (!try_lock()) {
        std::this_thread::yield();
    }
}

void EncMMap::unlock() const {
    assert(lock_.test_and_set(std::memory_order_acquire));
    lock_.clear(std::memory_order_release);
}

size_t EncMMap::get_size() const {
    return vmem_size_;
}

void* EncMMap::get_ptr() const {
    return base_;
}

void EncMMap::dont_need() const {
    mmapraw_->dont_need();
}

void EncMMap::mprotectd(unsigned char *ptr, size_t size, int prot) const {
    S_DEBUG_N("mprotect ptr: x%llX, size: %ld, prot: %s\n",
      ptr2ull(ptr), size, page_protection_to_string(prot));
    if (0 != mprotect(ptr, size, prot)) {
        S_DEBUG_E("mprotect failed. errno: %d, msg: %s\n", errno, strerror(errno));
    }

    size_t first_page_no = page_number(ptr);
    if (gu_likely(size == page_size_)) {
        vpage2protection_[first_page_no] = prot;
    } else {
        size_t pages_cnt = size/page_size_;
        pages_cnt = (size%page_size_) ? pages_cnt+1 : pages_cnt;
        memset(&(vpage2protection_[first_page_no]), prot, sizeof(int) * pages_cnt);
    }
}

// todo: maybe use PageGluer here as well?
void EncMMap::sync(void *addr, size_t length) const {
    unsigned char* addrU = static_cast<unsigned char*>(addr);

    S_DEBUG_N("sync() addr: %llX, length: %ld\n", ptr2ull(addr), length);

    size_t first_page_to_sync = page_number(addrU);
    unsigned char* vpage_end = addrU + length;
    size_t last_page_to_sync = page_number(vpage_end);

    // calculate the real lenght to sync. It is pages bound
    unsigned char* sync_addr_start = page_start(first_page_to_sync);
    size_t last_page_size = is_last_page(last_page_to_sync) ? last_page_size_ : page_size_;
    unsigned char* sync_addr_end = page_start(last_page_to_sync) + last_page_size;
    size_t real_sync_len = sync_addr_end - sync_addr_start;
    size_t sync_start_offset = base_ - addrU;

    lock();
    for (auto kv = vpage2ppage_.begin(); kv != vpage2ppage_.end(); ++kv) {
        size_t page_no = page_number(kv->first);
        if(page_no < first_page_to_sync || page_no > last_page_to_sync) {
            continue;
        }

        int protection = vpage2protection_[page_no];
        unsigned char* vpage_start = kv->first;

        S_DEBUG_N("sync page_no: %d, prot: %s (x%llX - x%llX)\n",
            page_no, page_protection_to_string(vpage2protection_[page_no]), ptr2ull(vpage_start), ptr2ull(vpage_start)+page_size_);

        if(protection == (PROT_READ | PROT_WRITE)) {
            // flush
            mprotectd(vpage_start, page_size_, PROT_READ);
            unsigned char* dst_ptr = mmapraw_ptr_ + page_no * page_size_;
            encrypt(dst_ptr, vpage_start, page_size_, page_no);
            S_DEBUG_N("    -> flushed\n");
            mprotectd(vpage_start, page_size_, default_page_protection_);
        }
    }
    unlock();
    // sync the underlying file
    // we need to sync whole alloc pages
    mmapraw_->sync(mmapraw_ptr_+sync_start_offset, real_sync_len);
 }

void EncMMap::sync() const {
    lock();
    for (auto kv = vpage2ppage_.begin(); kv != vpage2ppage_.end(); ++kv) {
        int page_no = page_number(kv->first);
        int protection = vpage2protection_[page_no];
        unsigned char* vpage_start = kv->first;
        S_DEBUG_N("sync() page_no: %d, prot: %s (x%llX - x%llX)\n",
            page_no, page_protection_to_string(vpage2protection_[page_no]), ptr2ull(vpage_start), ptr2ull(vpage_start)+page_size_);
        if(protection == (PROT_READ | PROT_WRITE)) {
            // flush
            mprotectd(vpage_start, page_size_, PROT_READ);
            unsigned char* dst_ptr = mmapraw_ptr_ + page_no*page_size_;
            encrypt(dst_ptr, vpage_start, page_size_, page_no);
            S_DEBUG_N("    -> flushed\n");
            mprotectd(vpage_start, page_size_, default_page_protection_);
        }
    }
    unlock();
    // sync the underlying file
    mmapraw_->sync();
}

void EncMMap::unmap() {
    // For RecordSet cache and GCache overflow pages this sync is not needed
    // if we unmap it means we will never map again, so we are not interested
    // with the content anymore.
    if (sync_on_destroy_) {
        sync();
    }

    EncMMapsRepository::DelEncMMap(this);

    if (munmap (mmap_ptr_, vmem_size_ + 2*page_size_) < 0)
    {
        gu_throw_error(errno) << "munmap(" << ptr2ull(mmap_ptr_) << ", " << vmem_size_ + 2*page_size_
                                << ") failed";
    }
    S_DEBUG_A("EncMMap::unmap() (x%llX - x%llX) (%ld bytes)\n",
        ptr2ull(base_), ptr2ull(base_) + vmem_size_, vmem_size_);
    base_ = nullptr;
    mapped_ = false;
}

void EncMMap::set_key(const std::string& key) {
    static unsigned char iv[Aes_ctr_encryptor::AES_BLOCK_SIZE] = {0};

    lock();
    assert(key.length() >= Aes_ctr_encryptor::FILE_KEY_LENGTH);
    unsigned const char *kkey = (unsigned const char*)key.c_str();
    encryptor_.close();
    decryptor_.close();
    encryptor_.open(kkey, iv);
    decryptor_.open(kkey, iv);

    // We just set the key. Cache may contain some data, which was decrypted
    // with old key. If we flush now, we will spoil the data.
    // Discard everything cached so far.
    mprotectd(base_, vmem_size_, PROT_NONE);
    memory_manager_.free_all();
    vpage2ppage_.clear();
    unlock();
}

void EncMMap::set_access_mode(AccessMode mode) {
    if (mode == READ) {
        read_ahead_cnt_ = 100;
        default_page_protection_ = PROT_READ;
    } else if (mode == READ_WRITE){
        read_ahead_cnt_ = 0;
        default_page_protection_ = PROT_READ | PROT_WRITE;
    }
}

/* When we need to flush pages it is the common situation that they are
   continous. In such a case glue them to avoid partial encryptions.
   The following class encapsulates gluing logic.  */
struct PageGluer {
    unsigned char* src_;
    unsigned char* dst_;
    size_t size_;
    int min_page_no_;
    int prev_page_no_;

    PageGluer(): src_(nullptr), dst_(nullptr), size_(0),
                 min_page_no_(-1), prev_page_no_(-1) {}
    bool glue(int page_no, unsigned char* src, unsigned char* dst, size_t size) {
        if (prev_page_no_ == -1) {
            prev_page_no_ = page_no;
            min_page_no_ = page_no;
            src_ = src;
            dst_ = dst;
            size_ = size;
            return true;
        }

        if (prev_page_no_ + 1 == page_no) {
            prev_page_no_ = page_no;
            size_ += size;
            return true;
        }
        return false;
    }

    void reset() {
        src_ = nullptr;
        dst_ = nullptr;
        size_ = 0;
        min_page_no_ = -1;
        prev_page_no_ = -1;
    }
};

// signal handler. The whole magic happens here
void EncMMap::handle_signal(siginfo_t* info) {
    if (!try_lock()) {
        S_DEBUG_N("encmmap collision\n");
        return;
    }

    S_DEBUG_N("handle_signal >>>>>>>>>>>\n");
    unsigned char* p = static_cast<unsigned char*>(info->si_addr);
    size_t req_page_no = page_number(p);
    unsigned char* req_page_start = page_start(p);

    S_DEBUG_N("this: x%llX, p: x%llX, req_page_no: %llu, (x%llX - x%llX)\n",
      ptr2ull(this), ptr2ull(p), req_page_no, ptr2ull(req_page_start), ptr2ull(req_page_start)+page_size_);

    assert(req_page_no < pages_cnt_);

    S_DEBUG_N("req_page_no: %llu, prot: %s\n",
      req_page_no, page_protection_to_string(vpage2protection_[req_page_no]));

    if (vpage2protection_[req_page_no] == PROT_NONE) {
        // Page is not mapped. Find free one
        auto p = memory_manager_.alloc();
        if (!p) {
            [[maybe_unused]] size_t freed_count = 0;
            [[maybe_unused]] size_t flushed_count = 0;
            unsigned char *vpage_start = nullptr;

            // free FLUSH_LIMIT pages, no more
            const static int FLUSH_LIMIT = 100;
            int limit = FLUSH_LIMIT;
            S_DEBUG_N("Freeing physical pages. allocated: %d\n", vpage2ppage_.size());

            PageGluer gluer;
            for (auto& kv : vpage2ppage_) {
                size_t page_no = page_number(kv.first);
                int protection = vpage2protection_[page_no];
                vpage_start = kv.first;
                S_DEBUG_N("free page_no: %d, prot: %s (x%llX - x%llX)\n",
                  page_no, page_protection_to_string(protection), ptr2ull(vpage_start), ptr2ull(vpage_start)+page_size_);
                freed_count++;
                if(protection == (PROT_READ | PROT_WRITE)) {
                    // flush
                    unsigned char* dst_ptr = mmapraw_ptr_ + page_no*page_size_;
                    mprotectd(vpage_start, page_size_, PROT_READ);

                    size_t page_size = is_last_page(page_no) ? last_page_size_ : page_size_;
                    if (gluer.glue(page_no, vpage_start, dst_ptr, page_size)) {
                        S_DEBUG_N("glued\n");
                        // Marking the page as not mapped should logically 
                        // be done in the loop below,
                        // but do it here to avoid page_no propagation/recalculation
                        vpage2protection_[page_no] = PROT_NONE;
                        flushed_count++;
                        S_DEBUG_N("    -> flushed\n");
                        if (--limit == 0) break;
                        continue;
                    }
                    S_DEBUG_N("not glued\n");

                    // there is at least one page in gluer
                    encrypt(gluer.dst_, gluer.src_, gluer.size_, gluer.min_page_no_);
                    gluer.reset();
                    gluer.glue(page_no, vpage_start, dst_ptr, page_size);

                    flushed_count++;
                    S_DEBUG_N("    -> flushed\n");
                }
                // Marking the page as not mapped should logically 
                // be done in the loop below,
                // but do it here to avoid page_no propagation/recalculation
                vpage2protection_[page_no] = PROT_NONE;

                if (--limit == 0) break;
            }

            if (gluer.size_ > 0) {
                encrypt(gluer.dst_, gluer.src_, gluer.size_, gluer.min_page_no_);
            }

            // do it again with the same limit, so we will free pages which were
            // synced above
            limit = FLUSH_LIMIT;
            for (auto kv = vpage2ppage_.begin(); kv != vpage2ppage_.end();) {
                vpage_start = kv->first;
                if (mmap(vpage_start, page_size_, PROT_NONE,
                    MAP_ANONYMOUS|MAP_PRIVATE|MAP_FIXED, -1, 0) == MAP_FAILED) {
                    S_DEBUG_E("unmap failed!");
                }

                // it is alread marked as PROT_NONE in the loop above

                memory_manager_.free(vpage2ppage_[vpage_start]);
                kv = vpage2ppage_.erase(kv);
                if (--limit == 0) break;
            }

            S_DEBUG_N("flused/freed: %ld / %ld\n", flushed_count, freed_count);
            p = memory_manager_.alloc();
            assert(p);
        }

        // this page
        unsigned char* srcPtr = mmapraw_ptr_ + req_page_no*page_size_;

        decrypt(p->ptr_, srcPtr, page_size_, req_page_no);

        if(MAP_FAILED == mmap(req_page_start, page_size_, default_page_protection_, MAP_SHARED|MAP_FIXED, p->fd_, p->offset_)) {
           S_DEBUG_E("mmap failed");
           assert(0); 
        }

        S_DEBUG_N("read req_page_no: %d (x%llX - x%llX) %s -> %s\n",
            req_page_no, ptr2ull(req_page_start),
            ptr2ull(req_page_start)+page_size_,
            page_protection_to_string(vpage2protection_[req_page_no]),
            page_protection_to_string(default_page_protection_));
        vpage2protection_[req_page_no] = default_page_protection_;
        vpage2ppage_[req_page_start] = p;

        // read ahead
        // This is useful for GCache recovery when the whole buffer is scanned
        [[maybe_unused]] size_t total_read_ahead = 0;
        for (size_t i = 0; i < read_ahead_cnt_; ++i) {
            req_page_no = req_page_no+1 < pages_cnt_ ? req_page_no+1 : 0;
            // only not mapped pages
            if (vpage2protection_[req_page_no] != PROT_NONE) {
                S_DEBUG_N("read ahead req_page_no: %d (x%llX - x%llX) already mapped. prot: %s\n",
                  req_page_no, ptr2ull(page_start(req_page_no)),
                  ptr2ull(page_start(req_page_no))+page_size_,
                  page_protection_to_string(vpage2protection_[req_page_no]));
                continue;
            }
            p = memory_manager_.alloc();
            if (!p) {
                // keep it simple for now. No swaping when read ahead.
                S_DEBUG_N("read ahead req_page_no: %d (x%llX - x%llX) no free pages.\n",
                  req_page_no, ptr2ull(page_start(req_page_no)),
                  ptr2ull(page_start(req_page_no))+page_size_);
                break;
            }
            unsigned char* srcPtr = mmapraw_ptr_ + req_page_no*page_size_;
            decrypt(p->ptr_, srcPtr, page_size_, req_page_no);
            req_page_start = page_start(req_page_no);

            mmap(req_page_start, page_size_, default_page_protection_, MAP_SHARED|MAP_FIXED, p->fd_, p->offset_);

            S_DEBUG_N("read ahead req_page_no: %d (x%llX - x%llX) %s -> %s\n",
                req_page_no, ptr2ull(req_page_start),
                ptr2ull(req_page_start)+page_size_,
                page_protection_to_string(vpage2protection_[req_page_no]),
                page_protection_to_string(default_page_protection_));
            vpage2protection_[req_page_no] = default_page_protection_;
            vpage2ppage_[req_page_start] = p;
            total_read_ahead++;
        }
        S_DEBUG_N("Read ahead %ld pages\n", total_read_ahead);
    } else if (vpage2protection_[req_page_no] == PROT_READ) {
        // page is mapped, just mark is as dirty
        mprotectd(req_page_start, page_size_, PROT_READ | PROT_WRITE);
        S_DEBUG_N("req_page_no: %d PROT_READ -> PROT_READ | PROT_WRITE\n", req_page_no);
    }
    S_DEBUG_N("handle_signal <<<<<<<<<\n");
    unlock();
}


}  // namespace
