/*
 * Copyright (C) 2010-2021 Codership Oy <info@codership.com>
 */

#include "gcache_rb_store.hpp"
#include "gcache_page_store.hpp"
#include "gcache_mem_store.hpp"
#include "gcache_limits.hpp"

#include <gu_logger.hpp>
#include <gu_throw.hpp>
#include <gu_progress.hpp>
#include <gu_hexdump.hpp>
#include <gu_hash.h>

#include <gu_enc_mmap_factory.hpp>
#include <gu_enc_utils.hpp>
#include <gu_crc.hpp>

#include <cassert>
#include <iostream> // std::cerr

namespace gcache
{
    class RecursiveLock {
        gu::RecursiveMutex& mtx_;
    public:
        RecursiveLock(gu::RecursiveMutex& mtx)
          : mtx_(mtx) {
            mtx_.lock();
          }
        ~RecursiveLock() {
            mtx_.unlock();
        }
    };

    static inline size_t check_size (size_t s)
    {
        return s + RingBuffer::pad_size() + sizeof(BufferHeader);
    }

    void
    RingBuffer::reset()
    {
        write_preamble(false);

        for (seqno2ptr_iter_t i = seqno2ptr_.begin(); i != seqno2ptr_.end(); ++i)
        {
            if (ptr2BH(*i)->ctx == BH_ctx_t(this)) {
                seqno2ptr_.erase(i);
            }
        }

        first_ = start_;
        next_  = start_;

        BH_clear (BH_cast(next_));

        size_free_ = size_cache_;
        size_used_ = 0;
        size_trail_= 0;

        /* When doing complete/full reset ensure that gcache is cleared too.
        Normally full reset take place when the cluster is re-boostrapped
        that assigns a new cluster-id. Gcache only stores seqno and not
        cluster-id so looking at gcache one can't say if the said entries
        belongs to old cluster or new bootstrapped cluster.
        For example: initially cluster had cluster-id = x
        gcache entries: x:1, x:2, x:3, x:4
        new bootstrap cluster has cluster-id = y and gcache is recovered
        then gcache entries would be y:1, y:2, y:3, y:4

        This doesn't make sense since entires are wrongly being associated
        with new cluster id.
        ref-tc: mysql-wsrep-features#9 */
        log_info << "Complete reset of the galera cache";
        memset(start_, 0, size_cache_);
        mmap_.sync();

//        mallocs_  = 0;
//        reallocs_ = 0;
    }

    void
    RingBuffer::constructor_common() {}

    RingBuffer::RingBuffer (ProgressCallback*  pcb,
                            const std::string& name,
                            size_t             size,
                            seqno2ptr_t&       seqno2ptr,
                            gu::UUID&          gid,
                            int const          dbg,
                            bool const         recover,
                            bool               encrypt,
                            size_t             encrypt_cache_page_size,
                            size_t             encrypt_cache_size,
                            gu::MasterKeyProvider* master_key_provider)
    :
        pcb_       (pcb),
        master_key_id_(0),
        const_mk_id_(),
        master_key_uuid_(),
        file_key_(),
        master_key_provider_(master_key_provider),
        mk_rotation_mutex_(),
        encrypt_   (encrypt && master_key_provider_ != nullptr),  // protects access to master_key_provider_ ptr as well
#ifdef PXC
#ifdef HAVE_PSI_INTERFACE
        fd_        (name, WSREP_PFS_INSTR_TAG_RINGBUFFER_FILE, check_size(size)),
#else
        fd_        (name, check_size(size)),
#endif /* HAVE_PSI_INTERFACE */
#else
        fd_        (name, check_size(size)),
#endif /* PXC */
        mmapptr_   (gu::MMapFactory::create(fd_, encrypt, encrypt_cache_page_size, check_size(encrypt_cache_size), false, static_cast<size_t>(PREAMBLE_LEN))),
        mmap_      (*mmapptr_),
        preamble_  (static_cast<char*>(mmap_.get_ptr())),
        header_    (reinterpret_cast<int64_t*>(preamble_ + PREAMBLE_LEN)),
        start_     (reinterpret_cast<uint8_t*>(header_   + HEADER_LEN)),
        end_       (reinterpret_cast<uint8_t*>(preamble_ + mmap_.get_size())),
        first_     (start_),
        next_      (first_),
        seqno2ptr_ (seqno2ptr),
        gid_       (gid),
#ifdef PXC
        max_used_  (first_ - static_cast<uint8_t*>(mmap_.get_ptr()) +
                    sizeof(BufferHeader)),
        freeze_purge_at_seqno_(SEQNO_ILL),
#endif /* PXC */
        size_cache_(end_ - start_ - sizeof(BufferHeader)),
        size_free_ (size_cache_),
        size_used_ (0),
        size_trail_(0),
//        mallocs_   (0),
//        reallocs_  (0),
        debug_     (dbg & DEBUG),
        open_      (true)
    {
        assert((uintptr_t(start_) % MemOps::ALIGNMENT) == 0);
        constructor_common ();
        if (encrypt_) {
            master_key_provider_->register_key_rotation_request_observer(
                [this]() {
                  return rotate_master_key();
                });
        } else if (master_key_provider_) {
            /* To be compatible with existing unit tests, master_key_provider
               constructor parameter is nullptr by default. Normal flow
               creates the provider in ReplicatorSMM constructor and injects
               it to GCache regardless of encryption being enabled or not.
               encrypt_ flag protects access to master_key_provider_ member
               in all places, besides this one, so we have to handle it
               explicitly. */
            master_key_provider_->register_key_rotation_request_observer(
                []() {
                  log_info << "GCache Encryption Master Key has not been rotated"
                           << " because GCache encryption is disabled.";
                  return false;
                }
            );
        }
        open_preamble(recover);
        BH_clear (BH_cast(next_));
    }

    RingBuffer::~RingBuffer ()
    {
        if (encrypt_) {
            master_key_provider_->register_key_rotation_request_observer([](){ return true; });
        }
        close_preamble();
        open_ = false;
        mmap_.sync();
    }

    static inline void
    empty_buffer(BufferHeader* const bh) //mark buffer as empty
    {
        bh->seqno_g = gcache::SEQNO_ILL;
    }

    bool
    buffer_is_empty(const BufferHeader* const bh)
    {
        return (SEQNO_ILL == bh->seqno_g);
    }

    /* discard all seqnos preceeding and including seqno */
    bool
    RingBuffer::discard_seqnos(seqno2ptr_t const& seq, seqno2ptr_t::iterator const i_begin,
                               seqno2ptr_t::iterator const i_end)
    {
        for (seqno2ptr_t::iterator i(i_begin); i != i_end;)
        {
#ifdef PXC
            /* Skip purge from this seqno onwards. */
            if (skip_purge(seq.index(i)))
                return false;
#endif /* PXC */

            seqno2ptr_t::iterator j(i);

            /* advance i to next set element skipping holes */
            do { ++i; } while ( i != i_end && !*i);

            BufferHeader* const bh(ptr2BH(*j));

            if (gu_likely (BH_is_released(bh)))
            {
                seqno2ptr_.erase (j);

                switch (bh->store)
                {
                case BUFFER_IN_RB:
                    discard(bh);
                    break;
                case BUFFER_IN_MEM:
                {
                    MemStore* const ms(static_cast<MemStore*>(BH_ctx(bh)));
                    ms->discard(bh);
                    break;
                }
                case BUFFER_IN_PAGE:
                {
                    Page*      const page (static_cast<Page*>(BH_ctx(bh)));
                    PageStore* const ps   (PageStore::page_store(page));
                    ps->discard(bh);
                    break;
                }
                default:
                    log_fatal << "Corrupt buffer header: " << bh;
                    abort();
                }
            }
            else
            {
                return false;
            }
        }

        return true;
    }

    // returns pointer to buffer data area or 0 if no space found
    BufferHeader*
    RingBuffer::get_new_buffer (size_type const size)
    {
        assert((size % MemOps::ALIGNMENT) == 0);
        assert_size_free();

        BH_assert_clear(BH_cast(next_));

        uint8_t* ret(next_);

        size_type const size_next(size + sizeof(BufferHeader));

        Limits::assert_size(size_next);

        if (ret >= first_) {
            assert (0 == size_trail_);
            // try to find space at the end
            size_t const end_size(end_ - ret);

            if (end_size >= size_next) {
                assert(size_free_ >= size);
                goto found_space;
            }
            else {
                // no space at the end, go from the start
                size_trail_ = end_size;
                ret = start_;
            }
        }

        assert (ret <= first_);

        if (size_t(first_ - ret) >= size_next) { assert(size_free_ >= size); }

        while (size_t(first_ - ret) < size_next) {
            // try to discard first buffer to get more space
            BufferHeader* bh = BH_cast(first_);

            if (!BH_is_released(bh) /* true also when first_ == next_ */ ||
                (bh->seqno_g > 0 && !discard_seqno (bh->seqno_g)))
            {
                // can't free any more space, so no buffer, next_ is unchanged
                // and revert size_trail_ if it was set above
                if (next_ >= first_) size_trail_ = 0;
                assert_sizes();
                return 0;
            }

            assert (first_ != next_);
            /* buffer is either discarded already, or it must have seqno */
            assert (SEQNO_ILL == bh->seqno_g);

            first_ += bh->size;
            assert_size_free();

            if (gu_unlikely(0 == (BH_cast(first_))->size))
            {
                // empty header: check if we fit at the end and roll over if not
                assert(first_ >= next_);
                assert(first_ >= ret);

                first_ = start_;
                assert_size_free();

                if (size_t(end_ - ret) >= size_next)
                {
                    assert(size_free_ >= size);
                    size_trail_ = 0;
                    goto found_space;
                }
                else
                {
                    size_trail_ = end_ - ret;
                    ret = start_;
                }
            }

            assert(ret <= first_);
        }

        assert (ret <= first_);

#ifndef NDEBUG
        if (size_t(first_ - ret) < size_next) {
            log_fatal << "Assertion ((first - ret) >= size_next) failed: "
                      << std::endl
                      << "first offt = " << (first_ - start_) << std::endl
                      << "next  offt = " << (next_  - start_) << std::endl
                      << "end   offt = " << (end_   - start_) << std::endl
                      << "ret   offt = " << (ret    - start_) << std::endl
                      << "size_next  = " << size_next         << std::endl;
            abort();
        }
#endif

    found_space:
        assert((uintptr_t(ret) % MemOps::ALIGNMENT) == 0);
        size_used_ += size;
        assert (size_used_ <= size_cache_);
        assert (size_free_ >= size);
        size_free_ -= size;

        BufferHeader* const bh(BH_cast(ret));
        bh->size    = size;
        bh->seqno_g = SEQNO_NONE;
        bh->flags   = 0;
        bh->store   = BUFFER_IN_RB;
        bh->ctx     = reinterpret_cast<BH_ctx_t>(this);
        next_ = ret + size;

#ifdef PXC
        size_t max_used =
            next_ - static_cast<uint8_t*>(mmap_.get_ptr()) + sizeof(BufferHeader);

        if (max_used > max_used_)
        {
            max_used_ = max_used;
        }
#endif /* PXC */

        assert((uintptr_t(next_) % MemOps::ALIGNMENT) == 0);
        assert (next_ + sizeof(BufferHeader) <= end_);
        BH_clear (BH_cast(next_));
        assert_sizes();

        return bh;
    }

    void*
    RingBuffer::malloc (size_type const size)
    {
        Limits::assert_size(size);

        void* ret(NULL);

        // We can reliably allocate continuous buffer which is 1/2
        // of a total cache space. So compare to half the space
        if (size <= (size_cache_ / 2) && size <= (size_cache_ - size_used_))
        {
            BufferHeader* const bh (get_new_buffer (size));

            BH_assert_clear(BH_cast(next_));
//            mallocs_++;

            if (gu_likely (0 != bh)) ret = bh + 1;
        }

        assert_sizes();

        return ret; // "out of memory"
    }

    void
    RingBuffer::free (BufferHeader* const bh)
    {
        assert(BH_is_released(bh));

        assert(size_used_ >= bh->size);
        size_used_ -= bh->size;

        if (SEQNO_NONE == bh->seqno_g)
        {
            empty_buffer(bh);
            discard (bh);
        }
    }

    void*
    RingBuffer::realloc (void* ptr, size_type const size)
    {
        Limits::assert_size(size);

        assert_sizes();
        assert (NULL != ptr);
        assert (size > 0);
        // We can reliably allocate continuous buffer which is twice as small
        // as total cache area. So compare to half the space
        if (size > (size_cache_ / 2)) return 0;

        BufferHeader* const bh(ptr2BH(ptr));

//        reallocs_++;

        // first check if we can grow this buffer by allocating
        // adjacent buffer
        {
            Limits::assert_size(bh->size);
            diff_type const adj_size(size - bh->size);
            if (adj_size <= 0) return ptr;

            uint8_t* const adj_ptr(reinterpret_cast<uint8_t*>(BH_next(bh)));
            if (adj_ptr == next_)
            {
                ssize_type const size_trail_saved(size_trail_);
                void* const adj_buf (get_new_buffer (adj_size));

                BH_assert_clear(BH_cast(next_));

                if (adj_ptr == adj_buf)
                {
                    bh->size = next_ - static_cast<uint8_t*>(ptr) +
                        sizeof(BufferHeader);
                    return ptr;
                }
                else // adjacent buffer allocation failed, return it back
                {
                    next_ = adj_ptr;
                    BH_clear (BH_cast(next_));
                    size_used_ -= adj_size;
                    size_free_ += adj_size;
                    if (next_ < first_) size_trail_ = size_trail_saved;
                }
            }
        }

        BH_assert_clear(BH_cast(next_));
        assert_sizes();

        // find non-adjacent buffer
        void* ptr_new = malloc (size);
        if (ptr_new != 0) {
            memcpy (ptr_new, ptr, bh->size - sizeof(BufferHeader));
            free (bh);
        }

        BH_assert_clear(BH_cast(next_));
        assert_sizes();

        return ptr_new;
    }

    void
    RingBuffer::estimate_space(bool zero_out)
    {
        /* Estimate how much space remains */
        if (first_ < next_)
        {
            /* start_  first_      next_    end_
             *   |       |###########|       |
             */
            size_used_ = next_ - first_;
            size_free_ = size_cache_ - size_used_;
            size_trail_ = 0;
            if (zero_out) {
                memset(next_, 0, (end_ - next_));
                memset(start_, 0, (first_ - start_));
                mmap_.sync();
            }
        }
        else
        {
            /* start_  next_       first_   end_
             *   |#######|           |#####| |
             *                              ^size_trail_ */
            assert(size_trail_ > 0);
            size_free_ = first_ - next_ + size_trail_ - sizeof(BufferHeader);
            size_used_ = size_cache_ - size_free_;
            if (zero_out) {
                memset(end_ - size_trail_, 0, size_trail_);
                memset(next_, 0, first_ - next_);
                mmap_.sync();
            }
        }

        assert_sizes();
        assert(size_free_ < size_cache_);
    }

    void
    RingBuffer::seqno_reset(bool zero_out)
    {
        write_preamble(false);

        if (size_cache_ == size_free_) return;

        /* Invalidate seqnos for all ordered buffers (so that they can't be
         * recovered on restart. Also find the last seqno'd RB buffer. */
        BufferHeader* bh(0);

        for (seqno2ptr_t::iterator i(seqno2ptr_.begin());
             i != seqno2ptr_.end(); ++i)
        {
            BufferHeader* const b(ptr2BH(*i));
            if (BUFFER_IN_RB == b->store)
            {
#ifndef NDEBUG
                if (!BH_is_released(b))
                {
                    log_fatal << "Buffer " << b << " is not released.";
                    assert(0);
                }
#endif
                b->seqno_g = SEQNO_NONE;
                bh = b;
            }
        }

        if (!bh) return; /* no seqno'd buffers in RB */

        assert(bh->size > 0);
        assert(BH_is_released(bh));

        /* Seek the first unreleased buffer.
         * This should be called in isolation, when all seqno'd buffers are
         * freed, and the only unreleased buffers should come only from new
         * configuration. There should be no seqno'd buffers after it. */

        size_t const old(size_free_);

        assert (0 == size_trail_ || first_ > next_);
        first_ = reinterpret_cast<uint8_t*>(bh);

        while (BH_is_released(bh)) // next_ is never released - no endless loop
        {
             first_ = reinterpret_cast<uint8_t*>(BH_next(bh));

             if (gu_unlikely (0 == bh->size && first_ != next_))
             {
                 // rollover
                 assert (first_ > next_);
                 first_ = start_;
             }

             bh = BH_cast(first_);
        }

        BH_assert_clear(BH_cast(next_));

        if (first_ == next_)
        {
            log_info << "GCache DEBUG: RingBuffer::seqno_reset(): full reset";
            /* empty RB, reset it completely */
            reset();
            return;
        }

        assert ((BH_cast(first_))->size > 0);
        assert (first_ != next_);
        assert ((BH_cast(first_))->seqno_g == SEQNO_NONE);
        assert (!BH_is_released(BH_cast(first_)));

        estimate_space(zero_out);

        log_info << "GCache DEBUG: RingBuffer::seqno_reset(): discarded "
                 << (size_free_ - old) << " bytes";

        /* There is a small but non-0 probability that some released buffers
         * are locked within yet unreleased aborted local actions.
         * Seek all the way to next_, invalidate seqnos and update size_free_ */

        assert(first_ != next_);
        assert(bh == BH_cast(first_));

        long total(1);
        long locked(0);

        bh = BH_next(bh);

        while (bh != BH_cast(next_))
        {
            if (gu_likely (bh->size > 0))
            {
                total++;

                if (bh->seqno_g != SEQNO_NONE)
                {
                    // either released or already discarded buffer
                    assert (BH_is_released(bh));
                    empty_buffer(bh);
                    discard (bh);
                    locked++;
                }
                else
                {
                    assert(!BH_is_released(bh));
                }

                bh = BH_next(bh);
            }
            else // rollover
            {
                assert (BH_cast(next_) < bh);
                bh = BH_cast(start_);
            }
        }

        log_info << "GCache DEBUG: RingBuffer::seqno_reset(): found "
                 << locked << '/' << total << " locked buffers";

        assert_sizes();

        if (next_ > first_ && first_ > start_) BH_clear(BH_cast(start_));
        /* this is needed to avoid rescanning from start_ on recovery */
    }

#ifdef PXC
    size_t RingBuffer::allocated_pool_size ()
    {
       return max_used_;
    }
#endif /* PXC */

    void
    RingBuffer::print (std::ostream& os) const
    {
        os  << "this: " << static_cast<const void*>(this)
            << "\nstart_ : " << BH_cast(start_)
            << "\nfirst_ : " << BH_cast(first_) << ", off: " << first_ - start_
            << "\nnext_  : " << BH_cast(next_) << ", off: " << next_  - start_
            << "\nsize   : " << size_cache_
            << "\nfree   : " << size_free_
            << "\nused   : " << size_used_;
    }

    std::string
    RingBuffer::generate_new_master_key(const std::string& key_name)
    {
        std::string key = master_key_provider_->get_key(key_name);
        if (!key.empty()) {
            return std::string();
        }

        // Key does not exist, so creation should succeed.
        if(master_key_provider_->create_key(key_name)) {
            return std::string();
        }

        key = master_key_provider_->get_key(key_name);
        if (key.empty()) {
            return std::string();
        }

        return key;
    }

    bool
    RingBuffer::rotate_master_key()
    {
        RecursiveLock mk_rotation_lock(mk_rotation_mutex_);
        std::string old_mk_name = gu::create_master_key_name(const_mk_id_, master_key_uuid_, master_key_id_);
        std::string old_mk = master_key_provider_->get_key(old_mk_name);
        if (old_mk.empty()) return true;

        // decrypt file_key_ with the old MK
        std::string unencrypted_file_key = gu::decrypt_key(gu::decode64(file_key_), old_mk);

        std::string new_mk_name = gu::create_master_key_name(const_mk_id_, master_key_uuid_, master_key_id_ + 1);
        std::string new_mk = generate_new_master_key(new_mk_name);

        if(new_mk.empty()) {
            log_info << "Generation of Master Key " << new_mk_name << " failed.";
            return true;
        }

        master_key_id_++;
        log_info << "Generated new Master Key: " << new_mk_name;

        // encrypt with new MK
        file_key_ = gu::encode64(gu::encrypt_key(unencrypted_file_key, new_mk));

        // store preamble
        write_preamble(false);

        log_info << "GCache Encryption Master Key has been rotated."
                 << " Current Master Key id: " << new_mk_name;
        return false;
    }

    std::string const RingBuffer::PR_KEY_VERSION   = "Version:";
    std::string const RingBuffer::PR_KEY_GID       = "GID:";
    std::string const RingBuffer::PR_KEY_SEQNO_MAX = "seqno_max:";
    std::string const RingBuffer::PR_KEY_SEQNO_MIN = "seqno_min:";
    std::string const RingBuffer::PR_KEY_OFFSET    = "offset:";
    std::string const RingBuffer::PR_KEY_SYNCED    = "synced:";
    std::string const RingBuffer::PR_KEY_ENCRYPTION_VERSION = "enc_version:";
    std::string const RingBuffer::PR_KEY_ENCRYPTED = "enc_encrypted:";
    std::string const RingBuffer::PR_KEY_MK_ID = "enc_mk_id:";
    std::string const RingBuffer::PR_KEY_MK_CONST_ID = "enc_mk_const_id:";
    std::string const RingBuffer::PR_KEY_MK_UUID = "enc_mk_uuid:";
    std::string const RingBuffer::PR_KEY_FILE_KEY = "enc_fk_id:";
    std::string const RingBuffer::PR_KEY_ENC_CRC = "enc_crc:";

    void
    RingBuffer::write_preamble(bool const synced)
    {
        RecursiveLock mk_rotation_lock(mk_rotation_mutex_);
        uint8_t* const preamble(reinterpret_cast<uint8_t*>(preamble_));

        std::ostringstream os;

        os << PR_KEY_VERSION << ' ' << VERSION << '\n';
        os << PR_KEY_GID << ' ' << gid_ << '\n';

        if (synced)
        {
            if (!seqno2ptr_.empty())
            {
                os << PR_KEY_SEQNO_MIN << ' '
                   << seqno2ptr_.index_front() << '\n';

                os << PR_KEY_SEQNO_MAX << ' '
                   << seqno2ptr_.index_back() << '\n';

                os << PR_KEY_OFFSET << ' ' << first_ - preamble << '\n';
            }
            else
            {
                os << PR_KEY_SEQNO_MIN << ' ' << SEQNO_ILL << '\n';
                os << PR_KEY_SEQNO_MAX << ' ' << SEQNO_ILL << '\n';
            }
        }

        os << PR_KEY_SYNCED << ' ' << synced << '\n';

        // Encryption info
        static const int ENCRYPTION_VERSION = 1;
        os << PR_KEY_ENCRYPTION_VERSION << ' ' << ENCRYPTION_VERSION << '\n';
        os << PR_KEY_ENCRYPTED << ' ' << encrypt_ << '\n';
        os << PR_KEY_MK_ID << ' ' << master_key_id_ << '\n';
        os << PR_KEY_MK_CONST_ID << ' ' << const_mk_id_ << '\n';
        os << PR_KEY_MK_UUID << ' ' << master_key_uuid_ << '\n';
        os << PR_KEY_FILE_KEY << ' ' << file_key_ << '\n';

        gu::CRC32C crc;
        crc.append(&ENCRYPTION_VERSION, sizeof(ENCRYPTION_VERSION));
        crc.append(&encrypt_, sizeof(encrypt_));
        crc.append(&master_key_id_, sizeof(master_key_id_));
        crc.append(const_mk_id_.ptr(), GU_UUID_LEN);
        crc.append(master_key_uuid_.ptr(), GU_UUID_LEN);
        crc.append(file_key_.c_str(), file_key_.length());
        uint32_t crc_val = crc.get();
        os << PR_KEY_ENC_CRC << ' ' << crc_val << '\n';

        os << '\n';

        ::memset(preamble_, '\0', PREAMBLE_LEN);

        size_t copy_len(os.str().length());
        if (copy_len >= PREAMBLE_LEN) copy_len = PREAMBLE_LEN - 1;

        ::memcpy(preamble_, os.str().c_str(), copy_len);

        mmap_.sync(preamble_, copy_len);
    }

    void
    RingBuffer::open_preamble(bool const do_recover)
    {
        int version(0); // used only for recovery on upgrade
        uint8_t* const preamble(reinterpret_cast<uint8_t*>(preamble_));
        long long seqno_max(SEQNO_ILL);
        long long seqno_min(SEQNO_ILL);
        off_t offset(-1);
        bool  synced(false);

        bool enc_encrypted(false);
        int enc_version(0);
        uint32_t enc_crc(0);
        bool force_reset(false);

        RecursiveLock mk_rotation_lock(mk_rotation_mutex_);
        {
            std::istringstream iss(preamble_);

            if (iss.fail())
                gu_throw_error(EINVAL) << "Failed to open preamble.";

            std::string line;
            while (getline(iss, line), iss.good())
            {
                std::istringstream istr(line);
                std::string key;

                istr >> key;

                if ('#' == key[0]) { /* comment line */ }
                else if (PR_KEY_VERSION   == key) istr >> version;
                else if (PR_KEY_GID       == key) istr >> gid_;
                else if (PR_KEY_SEQNO_MAX == key) istr >> seqno_max;
                else if (PR_KEY_SEQNO_MIN == key) istr >> seqno_min;
                else if (PR_KEY_OFFSET    == key) istr >> offset;
                else if (PR_KEY_SYNCED    == key) istr >> synced;
                else if (PR_KEY_ENCRYPTION_VERSION == key) istr >> enc_version;
                else if (PR_KEY_ENCRYPTED == key) istr >> enc_encrypted;
                else if (PR_KEY_MK_ID     == key) istr >> master_key_id_;
                else if (PR_KEY_MK_CONST_ID == key) istr >> const_mk_id_;
                else if (PR_KEY_MK_UUID    == key) istr >> master_key_uuid_;
                else if (PR_KEY_FILE_KEY  == key) istr >> file_key_;
                else if (PR_KEY_ENC_CRC   == key) istr >> enc_crc;
            }
        }

        if (version < 0 || version > 16)
        {
           log_warn << "Bogus version in GCache ring buffer preamble: "
                    << version << ". Assuming 0.";
           version = 0;
        }

        if (offset < -1 ||
            (preamble + offset + sizeof(BufferHeader)) > end_ ||
            (version >= 2 && offset >= 0 && (offset % MemOps::ALIGNMENT)))
        {
           log_warn << "Bogus offset in GCache ring buffer preamble: "
                    << offset << ". Assuming unknown.";
           offset = -1;
        }

        if (const_mk_id_ == GU_UUID_NIL) {
            const_mk_id_ = gu::UUID(0, 0);
            log_info << "Generated new GCache ID: " << const_mk_id_;
        }

        if (enc_encrypted != encrypt_) {
            // if we are switching enc <-> not enc, no point in recovering
            log_info << "Switching GCache encryption "
                     << (enc_encrypted ? "ON" : "OFF")
                     << " -> "
                     << (encrypt_ ? "ON" : "OFF")
                     << ". This forces GCache reset.";
            file_key_.clear();
            master_key_id_ = 0;
            master_key_uuid_ = gu::UUID();
            force_reset = true;
        }

        if (encrypt_) {
            /* Q: Why do we store master key ID in gcache.preamble instead of
               doing the whole key management (e.g rotation) on server side
               and only informing Galera about the new key?
               A: We need access to the master key when Galera is initialized
               to be able to perform GCache recovery. At this stage storage
               engines are not initialized yet.
               If we did key management on server side, we would need to store
               key name info somewhere. Wsrep_schema and dedicated table seems to
               be ideal place, but again, we don't have access to it when Galera
               starts, so another solution is keeping it in dedicated file.
               But having yet another file on server side if we have this preamble
               does not seem to be good idea. */
            uint32_t crc_val = 0;
            if (enc_crc != 0) {
                // we've got some CRC, check if encryption data is consistent
                gu::CRC32C crc;
                crc.append(&enc_version, sizeof(enc_version));
                crc.append(&enc_encrypted, sizeof(enc_encrypted));
                crc.append(&master_key_id_, sizeof(master_key_id_));
                crc.append(const_mk_id_.ptr(), GU_UUID_LEN);
                crc.append(master_key_uuid_.ptr(), GU_UUID_LEN);
                crc.append(file_key_.c_str(), file_key_.length());
                crc_val = crc.get();
            }
            if (crc_val != enc_crc) {
                log_warn << "Encryption header CRC mismatch."
                            << " Calculated: " << crc_val
                            << " Expected: " << enc_crc;
            }
            if (enc_crc == 0 || crc_val != enc_crc) {
                // No crc info (no header?) or crc mismatch.
                // This will trigger new file key generation and GCache reset
                file_key_.clear();
                // master key can be spoiled as well
                master_key_id_ = 0;
            }

            std::string mk;
            bool allow_retry = true;
            while (allow_retry) {
                std::string mk_name;
                if (master_key_id_ == 0 || master_key_uuid_ == GU_UUID_NIL) {
                    // no MasterKey. Generate the new one
                    master_key_uuid_ = gu::UUID(0,0);
                    master_key_id_ = 1;

                    mk_name = gu::create_master_key_name(const_mk_id_, master_key_uuid_, master_key_id_);

                    log_info << "Master Key does not exist. Generating the new one: " << mk_name;
                    /* The following call should generate new MK because we use new uuid.
                    If it returns empty string it means something wrong happens to the keyring.
                    Such a case will be caught by the 'if' after loop. */
                    mk = generate_new_master_key(mk_name);

                    // This is new key. Do not allow retry.
                    allow_retry = false;
                } else {
                    mk_name = gu::create_master_key_name(const_mk_id_, master_key_uuid_, master_key_id_);
                    mk = master_key_provider_->get_key(mk_name);

                    /* Check for the existence of next Master Key. If it exists it can be:
                    1. Previous rotation was interrupted after new MK generation, but before
                       writing preamble
                    2. We are starting from old backup. The Master Key is valid, but as there
                       were rotations in the meantime, next keys exist as well
                    In both cases the 'future' keys we see may be already compromised,
                    so we shouldn't use them.
                    In such a case generate new Master Key with new, unique ID and trigger
                    GCache reset.
                    Note: for simplicity we just generate new key and reset GCache. If necessary
                    it is possible to only rotate MK with forced use o new uuid, but do not 
                    overcomplicate for now. */
                    std::string next_mk_name = gu::create_master_key_name(const_mk_id_, master_key_uuid_, master_key_id_+1);
                    std::string next_mk = master_key_provider_->get_key(next_mk_name);
                    if (mk.empty()) {
                        log_info << "GCache is encrypted with Master Key: "
                                 << mk_name << " but the key is missing. "
                                 << "Generating the new one.";
                    } else if(!next_mk.empty()) {
                        log_info << "GCache Master Key " << mk_name << " exists, but next key "
                                 << next_mk_name
                                 << " (and probably more following as well) exists as well."
                                 << " It may be caused by interrupting of previous rotation"
                                 << " in the middle or by starting the server with old GCache."
                                 << " Generating brand new Master Key to avoid usage of potentially"
                                 << " compromised keys.";
                        mk.clear();
                    }
                }

                if (!mk.empty()) break;

                // MK not found. Generate the new one, but try only once.
                master_key_id_ = 0;
                file_key_.clear();
            }

            if (mk.empty()) {
                std::stringstream oss;
                oss << "GCache encryption Master Key not generated or not found. "
                    << "Please check the keyring is loaded or disable GCache "
                    << "encryption. Aborting.";
                throw gu::Exception(oss.str(), 0);
            }

            // 2. Decrypt file_key_ (or generate the new one)
            std::string unencrypted_file_key;

            if (file_key_.empty()) {
                // no file key. Generate the new one and force GCache reset.
                log_info << "File Key empty. Generating the new one. This forces GCache reset.";
                unencrypted_file_key = gu::generate_random_key();
                file_key_ = gu::encode64(gu::encrypt_key(unencrypted_file_key, mk));
                force_reset = true;
            } else {
                unencrypted_file_key = gu::decrypt_key(gu::decode64(file_key_), mk);
            }
            // 3. pass file key to the mmap
            // Yes, I know this is ugly and IMMap should not have set_key() method,
            // and we should setup file key when MMapEnc decorator is created by factory,
            // but we have file key now, not earlier when mmap is created,
            // mmap_ is the reference, so no way to wrap it here
            // so just to not touch too much of the original code, and to not couple
            // RingBuffer class with MMapEnc class...
            mmap_.set_key(unencrypted_file_key);
        }

        log_info << "GCache DEBUG: opened preamble:"
                 << "\nVersion: " << version
                 << "\nUUID: " << gid_
                 << "\nSeqno: " << seqno_min << " - " << seqno_max
                 << "\nOffset: " << offset
                 << "\nSynced: " << synced
                 << "\nEncVersion: " << enc_version
                 << "\nEncrypted: " << encrypt_
                 << "\nMasterKeyConst UUID: " << const_mk_id_
                 << "\nMasterKey UUID: " << master_key_uuid_
                 << "\nMasterKey ID: " << master_key_id_;

        if (force_reset){
            log_info << "GCache ring buffer forced reset";
            reset();
        }
        else if (do_recover)
        {
            if (gid_ != gu::UUID())
            {
                log_info << "Recovering GCache ring buffer: version: " <<version
                         << ", UUID: " << gid_ << ", offset: " << offset;

                try
                {
                    recover(offset - (start_ - preamble), version);
                }
                catch (gu::Exception& e)
                {
                    log_warn << "Failed to recover GCache ring buffer: "
                             << e.what();
                    reset();
                }
            }
            else
            {
                log_info << "Skipped GCache ring buffer recovery: could not "
                    "determine history UUID.";
            }
        }

        write_preamble(false);
    }

    void
    RingBuffer::close_preamble()
    {
        write_preamble(true);
    }

    /* Helper callback class to specilize for different progress types */
    template <typename T>
    class recover_progress_callback : public gu::Progress<T>::Callback
    {
    public:
        recover_progress_callback(gcache::ProgressCallback* pcb)
            : pcb_(pcb)
        {}
        ~recover_progress_callback() {}
        void operator()(T const total, T const done)
        {
            if (pcb_) (*pcb_)(total, done);
        }
    private:
        recover_progress_callback(const recover_progress_callback&);
        recover_progress_callback& operator=(recover_progress_callback);
        ProgressCallback* pcb_;
    };

    seqno_t
    RingBuffer::scan(off_t const offset, int const scan_step)
    {
        int segment_scans(0);
        seqno_t seqno_max(SEQNO_ILL);
        uint8_t* ptr;
        BufferHeader* bh;
        size_t collision_count(0);
        seqno_t erase_up_to(-1);
        uint8_t* segment_start(start_);
        uint8_t* segment_end(end_ - sizeof(BufferHeader));

        mmap_.set_access_mode(gu::IMMap::READ);
        /* start at offset (first segment) if we know it and it is valid */
        if (offset >= 0)
        {
            assert(0 == (offset % scan_step));

            if (start_ + offset + sizeof(BufferHeader) < segment_end)
                /* we know exaclty where the first segment starts */
                segment_start = start_ + offset;
            else
                /* first segment is completely missing, advance scan count */
                segment_scans = 1;
        }

        recover_progress_callback<ptrdiff_t> scan_progress_callback(pcb_);
        gu::Progress<ptrdiff_t> progress(&scan_progress_callback,
                                         "GCache::RingBuffer initial scan",
                                         " bytes", end_ - start_, 1<<22/*4Mb*/);

        while (segment_scans < 2)
        {
            segment_scans++;

            ptr = segment_start;
            bh = BH_cast(ptr);

#define GCACHE_SCAN_BUFFER_TEST                                 \
            (BH_test(bh) && bh->size > 0 &&                     \
             ptr + bh->size <= segment_end &&                   \
             BH_test(BH_cast(ptr + bh->size)))

#define GCACHE_SCAN_ADVANCE(amount)             \
            ptr += amount;                      \
            progress.update(amount);            \
            bh = BH_cast(ptr);


            while (GCACHE_SCAN_BUFFER_TEST)
            {
                assert((uintptr_t(bh) % scan_step) == 0);

                bh->flags |= BUFFER_RELEASED;
                bh->ctx    = uint64_t(this);

                seqno_t const seqno_g(bh->seqno_g);

                if (gu_likely(seqno_g > 0))
                {
                    bool const collision(
                        seqno_g <= seqno_max &&
                        seqno_g >= seqno2ptr_.index_begin() &&
                        seqno2ptr_[seqno_g] != seqno2ptr_t::null_value());

                    if (gu_unlikely(collision))
                    {
                        collision_count++;

                        /* compare two buffers */
                        seqno2ptr_t::const_reference old_ptr
                            (seqno2ptr_[seqno_g]);
                        BufferHeader* const old_bh
                            (old_ptr ? ptr2BH(old_ptr) : NULL);

                        bool const same_meta(NULL != old_bh &&
                            bh->seqno_g == old_bh->seqno_g  &&
                            bh->size    == old_bh->size     &&
                            bh->flags   == old_bh->flags);

                        const void* const new_ptr(static_cast<void*>(bh+1));

                        uint8_t cs_old[16] = { 0, };
                        uint8_t cs_new[16] = { 0, };
                        if (same_meta)
                        {
                            gu_fast_hash128(old_ptr,
                                            old_bh->size - sizeof(BufferHeader),
                                            cs_old);
                            gu_fast_hash128(new_ptr,
                                            bh->size - sizeof(BufferHeader),
                                            cs_new);
                        }

                        bool const same_data(same_meta &&
                                             !::memcmp(cs_old, cs_new,
                                                       sizeof(cs_old)));
                        std::ostringstream msg;

                        msg << "Attempt to reuse the same seqno: " << seqno_g
                            << ". New ptr = " << new_ptr << ", " << bh
                            << ", cs: " << gu::Hexdump(cs_new, sizeof(cs_new))
                            << ", previous ptr = " << old_ptr;

                        empty_buffer(bh); // this buffer is unusable
                        assert(BH_is_released(bh));

                        if (old_bh != NULL)
                        {
                            msg << ", " << old_bh << ", cs: "
                                << gu::Hexdump(cs_old,sizeof(cs_old));

                            if (!same_data) // no way to choose which is correct
                            {
                                empty_buffer(old_bh);
                                assert(BH_is_released(old_bh));

                                if (erase_up_to < seqno_g) erase_up_to =seqno_g;
                            }
                        }

                        log_info << msg.str();

                        if (same_data) {
                            log_info << "Contents are the same, discarding "
                                     << new_ptr;
                        } else {
                            log_info << "Contents differ. Discarding both.";
                        }
                    }
                    else
                    {
                        try
                        {
                            seqno2ptr_.insert(seqno_g, bh + 1);
                        }
                        catch (std::exception& e)
                        {
                            seqno_t const sb(seqno2ptr_.empty() ? SEQNO_ILL :
                                             seqno2ptr_.index_begin());
                            seqno_t const se(seqno2ptr_.empty() ? SEQNO_ILL :
                                             seqno2ptr_.index_end());
                            log_warn << "Exception while mapping writeset "
                                     << bh << " into [" << sb << ", " << se
                                     << "): '" << e.what()
                                     << "'. Aborting GCache recovery.";
                            /* Buffer scanning was interrupted ungracefully -
                             * this means that we failed to recover the most
                             * recent writesets. As such anything that was
                             * potentially recovered before is useless.
                             * This will cause full cache reset in recover() */
                            seqno2ptr_.clear(SEQNO_ILL);

                            BH_clear(bh); // to pacify assert() below
                            next_ = ptr;
                            goto out;
                        }
                        seqno_max = std::max(seqno_g, seqno_max);
                    }
                }

                GCACHE_SCAN_ADVANCE(bh->size);
            }

            if (!BH_is_clear(bh))
            {
                if (start_ == segment_start && ptr != first_ &&
                    ptr + bh->size != first_)
                    /* ptr + bh->size == first_ means that there is only one
                     * segment starting at first_ and the space between start_
                     * and first_ occupied by discarded buffers. */
                {
                    log_warn << "Failed to scan the last segment to the end. "
                            "Last events may be missing. Last recovered event: "
                             << gid_ << ':' << seqno_max;
                }

                /* end of segment, close it */
                BH_clear(bh);
            }

            if (offset > 0 && segment_start == start_ + offset)
            {
                /* started with the first segment, jump to the second one */
                assert(1 == segment_scans);
                first_ = segment_start;
                size_trail_ = end_ - ptr;
                // there must be at least one buffer header between the segments
                segment_end = segment_start - sizeof(BufferHeader);
                segment_start = start_;
            }
            else if (offset < 0 && segment_start == start_)
            {
                /* started with the second segment, try to find the first one */
                assert(1 == segment_scans);
                next_ = ptr;
                GCACHE_SCAN_ADVANCE(sizeof(BufferHeader));

                while (ptr + sizeof(BufferHeader) < end_ &&
                       !GCACHE_SCAN_BUFFER_TEST)
                {
                    GCACHE_SCAN_ADVANCE(scan_step);
                }

                if (GCACHE_SCAN_BUFFER_TEST)
                {
                    /* looks like a valid buffer, a beginning of a segment */
                    segment_start = ptr;
                    first_ = segment_start;
                }
                else if (ptr + sizeof(BufferHeader) >= end_)
                {
                    /* perhaps it was a single segment starting at start_ */
                    first_ = start_;
                    break;
                }
                else
                {
                    assert(0);
                }
            }
            else if (offset == 0 && segment_start == start_)
            {
                /* single segment case */
                assert(1 == segment_scans);
                first_ = segment_start;
                next_ = ptr;
                break;
            }
            else
            {
                assert(2 == segment_scans);
                assert(offset != 0);

                if (offset >= 0) next_ = ptr; /* end of the second segment */

                assert(first_ >= start_ && first_ < end_);
                assert(next_  >= start_ && next_  < end_);

                if (offset < 0 && segment_start > start_)
                {
                    /* first (end) segment was scanned last, estimate trail */
                    size_trail_ = end_ - ptr;
                }
                else if (offset > 0 && next_ > first_)
                {
                    size_trail_ = 0;
                }
            }
#undef GCACHE_SCAN_BUFFER_TEST
#undef GCACHE_SCAN_ADVANCE
        } // while (segment_scans < 2)
    out:
        assert(BH_is_clear(BH_cast(next_)));
        progress.finish();

        if (debug_)
        {
            log_info
                << "RB: end of scan(): seqno2ptr: "
                << seqno2ptr_.index_begin() << " - " << seqno2ptr_.index_end()
                << ", seqno_max: " << seqno_max;
            log_info << "RB: " << *this;
            dump_map();
        }

        mmap_.set_access_mode(gu::IMMap::READ_WRITE);
        return erase_up_to;
    }

    static bool assert_ptr_seqno(seqno2ptr_t& map,
                                 const void* const ptr,
                                 seqno_t     const seqno)
    {
        const BufferHeader* const bh(ptr2BH(ptr));
        if (bh->seqno_g != seqno)
        {
            assert(0);
            map.clear(SEQNO_NONE);
            return true;
        }
        return false;
    }

    void
    RingBuffer::recover(off_t const offset, int version)
    {
        static const char* const diag_prefix ="Recovering GCache ring buffer: ";

        /* scan the buffer and populate seqno2ptr map */
        seqno_t const lowest(scan(offset, version > 0 ? MemOps::ALIGNMENT : 1)
                             + 1);
        /* lowest is the lowest valid seqno based on collisions during scan */

        if (!seqno2ptr_.empty())
        {
            assert(next_ <= first_ || size_trail_ == 0);
            assert(next_ >  first_ || size_trail_ >  0);

            /* find the last gapless seqno sequence */
            seqno2ptr_t::reverse_iterator r(seqno2ptr_.rbegin());
            assert(*r);
            seqno_t const seqno_max(seqno2ptr_.index_back());
            seqno_t       seqno_min(seqno2ptr_.index_front());

            /* need to search for seqno gaps */
            assert(seqno_max >= lowest);
            if (lowest == seqno_max)
            {
                seqno2ptr_.clear(SEQNO_NONE);
                goto full_reset;
            }

            seqno_min = seqno_max;
            if (assert_ptr_seqno(seqno2ptr_, *r, seqno_min)) goto full_reset;

            /* At this point r and seqno_min both point at the last element in
             * the map. Scan downwards and bail out on the first hole.*/
            ++r;
            for (; r != seqno2ptr_.rend() && *r && seqno_min > lowest; ++r)
            {
                --seqno_min;
                if (assert_ptr_seqno(seqno2ptr_, *r,seqno_min)) goto full_reset;
            }
            /* At this point r points to one below seqno_min */

            log_info << diag_prefix << "found gapless sequence " << seqno_min
                     << '-' << seqno_max;

            if (r != seqno2ptr_.rend())
            {
                assert(seqno_min > seqno2ptr_.index_begin());
                log_info << diag_prefix << "discarding seqnos "
                         << seqno2ptr_.index_begin() << '-' << seqno_min - 1;

                /* clear up seqno2ptr map */
                for (; r != seqno2ptr_.rend(); ++r)
                {
                    if (*r) empty_buffer(ptr2BH(*r));
                }
                seqno2ptr_.erase(seqno2ptr_.begin(),seqno2ptr_.find(seqno_min));
            }
            assert(seqno2ptr_.size() > 0);

            /* trim first_: start with the current first_ and scan forward to
             * the first non-empty buffer. */
            BufferHeader* bh(BH_cast(first_));
            assert(bh->size > sizeof(BufferHeader));
            while (bh->seqno_g == SEQNO_ILL)
            {
                assert(bh->size > sizeof(BufferHeader));

                bh = BH_next(bh);

                if (gu_unlikely(0 == bh->size)) bh = BH_cast(start_);// rollover
            }
            first_ = reinterpret_cast<uint8_t*>(bh);

            /* trim next_: start with the last seqno and scan forward up to the
             * current next_. Update to the end of the last non-empty buffer. */
            bh = ptr2BH(seqno2ptr_.back());
            BufferHeader* last_bh(bh);
            while (bh != BH_cast(next_))
            {
                if (gu_likely(bh->size) > 0)
                {
                    bool const inconsistency(
                        BH_next(bh) > BH_cast(end_ - sizeof(BufferHeader)) ||
                        bh->ctx != BH_ctx_t(this)
                        );

                    if (gu_unlikely(inconsistency))
                    {
                        assert(0);
                        log_warn << diag_prefix << "Corrupt buffer leak1: "<<bh;
                        goto full_reset;
                    }

                    assert(bh->size > sizeof(BufferHeader));

                    if (bh->seqno_g > 0) last_bh = bh;

                    bh = BH_next(bh);
                }
                else
                {
                    bh = BH_cast(start_); // rollover
                }
            }
            next_ = reinterpret_cast<uint8_t*>(BH_next(last_bh));

            /* Even if previous buffers were not aligned, make sure from
             * now on they are - adjust next_ pointer and last buffer size */
            if (uintptr_t(next_) % MemOps::ALIGNMENT)
            {
                uint8_t* const n(MemOps::align_ptr(next_));
                assert(n > next_);
                size_type const size_diff(n - next_);
                assert(size_diff < MemOps::ALIGNMENT);
                assert(last_bh->size > 0);
                last_bh->size += size_diff;
                next_ = n;
                assert(BH_next(last_bh) == BH_cast(next_));
            }
            assert((uintptr_t(next_) % MemOps::ALIGNMENT) == 0);
            BH_clear(BH_cast(next_));

            /* at this point we must have at least one seqno'd buffer */
            assert(next_ != first_);

            /* as a result of trimming, trailing space may be gone */
            if (first_ < next_) size_trail_ = 0;
            else assert(size_trail_ >= sizeof(BufferHeader));

            estimate_space();
#if 0
#ifdef PXC
            /* On graceful shutdown all the active buffers are released
            so on recovery size_used_ = 0.
            size_cache_ = size_free_ + size_used_ + releasebutnotdiscarded */
            size_used_ = 0;
#endif /* PXC */
#endif

            /* now discard all the locked-in buffers (see seqno_reset()) */
            size_t total(0);
            size_t locked(0);

            {
                recover_progress_callback<size_t>
                    unused_progress_callback(pcb_);
                gu::Progress<size_t> progress(
                    &unused_progress_callback,
                    "GCache::RingBuffer unused buffers scan",
                    " bytes", size_used_, 1<<22 /* 4Mb */);

                bh = BH_cast(first_);
                while (bh != BH_cast(next_))
                {
                    if (gu_likely(bh->size > 0))
                    {
                        bool const inconsistency(
                            BH_next(bh) > BH_cast(end_ - sizeof(BufferHeader))
                            ||
                            bh->ctx != BH_ctx_t(this)
                            );

                        if (gu_unlikely(inconsistency))
                        {
                            assert(0);
                            log_warn << diag_prefix << "Corrupt buffer leak2: "
                                     << bh;
                            goto full_reset;
                        }

                        total++;

                        if (gu_likely(bh->seqno_g > 0))
                        {
                            free(bh); // on recovery no buffer is used
                        }
                        else
                        {
                            /* anything that is not ordered must be discarded */
                            assert(SEQNO_NONE == bh->seqno_g ||
                                   SEQNO_ILL  == bh->seqno_g);
                            locked++;
                            empty_buffer(bh);
                            discard(bh);
                            size_used_ -= bh->size;
                            // size_free_ is taken care of in discard()
                        }

                        bh = BH_next(bh);
                    }
                    else
                    {
                        bh = BH_cast(start_); // rollover
                    }

                    progress.update(bh->size);
                }

                progress.finish();
            }

            /* No buffers on recovery should be in used state */
            assert(0 == size_used_);

            log_info << diag_prefix << "found "
                     << locked << '/' << total << " locked buffers";
            log_info << diag_prefix << "free space: "
                     << size_free_ << '/' << size_cache_;

            assert_sizes();

            if (debug_)
            {
                log_info << *this;
                dump_map();
            }
        }
        else
        {
        full_reset:
            log_info << diag_prefix <<"Recovery failed, need to do full reset.";
            reset();
        }
    }

    static void
    print_chain(const uint8_t* const rb_start, const uint8_t* const chain_start,
                const uint8_t* const chain_end, size_t const count,
                const char* const type_str)
    {
        ptrdiff_t const start_off(chain_start - rb_start);
        ptrdiff_t const end_off(chain_end - rb_start);
        std::cerr <<  start_off << "\t" << end_off << "\t"
                  << end_off - start_off << "\t" << count << "\t"
                  << type_str << std::endl;
    }

    void
    RingBuffer::dump_map() const
    {
        enum chain_t
        {
            ORDERED,
            UNORDERED,
            RELEASED,
            NONE
        };

        static const char* chain_str[] =
            { "ORDERED", "UNORDERED", "RELEASED", "NONE" };

        size_t chain_size[] = { 0, 0, 0, 0 };
        size_t chain_count[] = { 0, 0, 0, 0 };

        chain_t chain(NONE);
        const uint8_t* chain_start;
        size_t count;

        bool next(false);
        const uint8_t* ptr(start_);
        const BufferHeader* bh(BH_const_cast(ptr));

        log_info << "RB start_";
        log_info << bh;
        for (int i(0); i < 2; i++)
        {
            while (!BH_is_clear(bh))
            {
                if (first_ == ptr && i == 0)
                {
                    goto first; // rare situation when there is only
                    // one segment in the start/middle
                }

                size_t const offset(bh->size);
                chain_t const typ(bh->seqno_g >= 0 ? ORDERED : UNORDERED);
                if (chain != typ)
                { // new chain starts
                    if (chain != NONE)
                    { // old chain ends
                        print_chain(start_, chain_start, ptr, count,
                                    chain_str[chain]);
                        chain_count[chain] += count;
                    }
                    chain = typ;
                    chain_start = ptr;
                    count = 0;
                }
                count++;
                chain_size[typ] += offset;
                chain_size[RELEASED] += offset * BH_is_released(bh);
                chain_count[RELEASED] += BH_is_released(bh);

                ptr += offset;
                bh = BH_const_cast(ptr);
            }
            // old chain ends
            print_chain(start_, chain_start, ptr, count, chain_str[chain]);
            chain_count[chain] += count;
            if (1 == i) break; // final segment read

            log_info << "RB next_";
            log_info << bh << ", off: " << ptr - start_;
            next = true;

            log_info << "RB middle gap: " << first_ - ptr;

            ptr = first_;
            bh = BH_const_cast(ptr);
        first:
            chain = NONE;
            count = 0;
            log_info << "RB first_";
            log_info << bh << ", off: " << ptr - start_;
        }

        if (!next)
        {
            log_info << "RB next_";
        }
        else
        {
            log_info << "RB rollover";
        }
        log_info << bh << ", off: " << ptr - start_;
        log_info << "RB trailing space: " << end_ - ptr;

        log_info << "RB space usage:"
                 << "\nORDERED  : " << chain_size[ORDERED]
                 << "\nUNORDERED: " << chain_size[UNORDERED]
                 << "\nRELEASED : " << chain_size[RELEASED]
                 << "\nNONE     : " << chain_size[NONE];
        log_info << "RB buf counts:"
                 << "\nORDERED  : " << chain_count[ORDERED]
                 << "\nUNORDERED: " << chain_count[UNORDERED]
                 << "\nRELEASED : " << chain_count[RELEASED]
                 << "\nNONE     : " << chain_count[NONE];
    }
} /* namespace gcache */
