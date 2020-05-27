#include <algorithm>
#include <assert.h>
#include <cstdlib>
#include <cstring>

#include <libcopp/utils/config/compiler_features.h>
#include <libcopp/utils/config/libcopp_build_features.h>
#include <libcopp/utils/errno.h>
#include <libcopp/utils/std/explicit_declare.h>

#include <libcopp/coroutine/coroutine_context.h>

#if defined(UTIL_CONFIG_THREAD_LOCAL)
// using thread_local
#else
#include <pthread.h>
#endif

#ifdef LIBCOPP_MACRO_USE_SEGMENTED_STACKS
extern "C" {
void __splitstack_getcontext(void *[COPP_MACRO_SEGMENTED_STACK_NUMBER]);

void __splitstack_setcontext(void *[COPP_MACRO_SEGMENTED_STACK_NUMBER]);

void __splitstack_releasecontext(void *[COPP_MACRO_SEGMENTED_STACK_NUMBER]);

void __splitstack_block_signals_context(void *[COPP_MACRO_SEGMENTED_STACK_NUMBER], int *, int *);
}
#endif

namespace copp {
    struct libcopp_inner_api_helper {
        typedef coroutine_context::jump_src_data_t jump_src_data_t;

        static UTIL_FORCEINLINE void set_caller(coroutine_context *src, const fcontext::fcontext_t &fctx) {
            if (UTIL_CONFIG_NULLPTR != src) {
                src->caller_ = fctx;
            }
        }

        static UTIL_FORCEINLINE void set_callee(coroutine_context *src, const fcontext::fcontext_t &fctx) {
            if (UTIL_CONFIG_NULLPTR != src) {
                src->callee_ = fctx;
            }
        }

#ifdef LIBCOPP_MACRO_USE_SEGMENTED_STACKS
        static UTIL_FORCEINLINE void splitstack_swapcontext(EXPLICIT_UNUSED_ATTR stack_context &from_sctx,
                                                            EXPLICIT_UNUSED_ATTR stack_context &       to_sctx,
                                                            libcopp_inner_api_helper::jump_src_data_t &jump_transfer) {
            if (UTIL_CONFIG_NULLPTR != jump_transfer.from_co) {
                __splitstack_getcontext(jump_transfer.from_co->callee_stack_.segments_ctx);
                if (&from_sctx != &jump_transfer.from_co->callee_stack_) {
                    memcpy(&from_sctx.segments_ctx, &jump_transfer.from_co->callee_stack_.segments_ctx, sizeof(from_sctx.segments_ctx));
                }
            } else {
                __splitstack_getcontext(from_sctx.segments_ctx);
            }
            __splitstack_setcontext(to_sctx.segments_ctx);
        }
#endif

        static void coroutine_context_callback(::copp::fcontext::transfer_t src_ctx) {
            assert(src_ctx.data);
            if (UTIL_CONFIG_NULLPTR == src_ctx.data) {
                abort();
                // return; // clang-analyzer will report "Unreachable code"
            }

            // copy jump_src_data_t in case it's destroyed later
            jump_src_data_t jump_src = *reinterpret_cast<jump_src_data_t *>(src_ctx.data);

            // this must in a coroutine
            coroutine_context *ins_ptr = jump_src.to_co;
            assert(ins_ptr);
            if (UTIL_CONFIG_NULLPTR == ins_ptr) {
                abort();
                // return; // clang-analyzer will report "Unreachable code"
            }

            // update caller of to_co
            ins_ptr->caller_ = src_ctx.fctx;

            // save from_co's fcontext and switch status
            if (UTIL_CONFIG_NULLPTR != jump_src.from_co) {
                jump_src.from_co->callee_ = src_ctx.fctx;
            }

            // this_coroutine
            coroutine_context_base::set_this_coroutine_base(ins_ptr);

            // run logic code
#if defined(LIBCOPP_MACRO_ENABLE_STD_EXCEPTION_PTR) && LIBCOPP_MACRO_ENABLE_STD_EXCEPTION_PTR
            try {
#endif
                ins_ptr->run_and_recv_retcode(jump_src.priv_data);
#if defined(LIBCOPP_MACRO_ENABLE_STD_EXCEPTION_PTR) && LIBCOPP_MACRO_ENABLE_STD_EXCEPTION_PTR
            } catch(...) {
                ins_ptr->unhandle_exception_ = std::current_exception();
            }
#endif

            ins_ptr->flags_ |= coroutine_context::flag_t::EN_CFT_FINISHED;
            // add memory fence to flush flags_(used in is_finished())
            // LIBCOPP_UTIL_LOCK_ATOMIC_THREAD_FENCE(libcopp::util::lock::memory_order_release);

            // jump back to caller
            ins_ptr->yield();
        }
    };
    /**
     * @brief call platform jump to asm instruction
     * @param to_fctx jump to function context
     * @param from_sctx jump from stack context(only used for save segment stack)
     * @param to_sctx jump to stack context(only used for set segment stack)
     * @param jump_transfer jump data
     */
    static inline void jump_to(fcontext::fcontext_t &to_fctx, EXPLICIT_UNUSED_ATTR stack_context &from_sctx,
                               EXPLICIT_UNUSED_ATTR stack_context &       to_sctx,
                               libcopp_inner_api_helper::jump_src_data_t &jump_transfer) LIBCOPP_MACRO_NOEXCEPT {

        copp::fcontext::transfer_t                 res;
        libcopp_inner_api_helper::jump_src_data_t *jump_src;
        // int from_status;
        // bool swap_success;
        // can not use any more stack now
        // can not initialize those vars here

#ifdef LIBCOPP_MACRO_USE_SEGMENTED_STACKS
        assert(&from_sctx != &to_sctx);
        // ROOT->A: jump_transfer.from_co == UTIL_CONFIG_NULLPTR, jump_transfer.to_co == A, from_sctx == A.caller_stack_, skip backup
        // segments A->B.start(): jump_transfer.from_co == A, jump_transfer.to_co == B, from_sctx == B.caller_stack_, backup segments
        // B.yield()->A: jump_transfer.from_co == B, jump_transfer.to_co == UTIL_CONFIG_NULLPTR, from_sctx == B.callee_stack_, skip backup
        // segments
        libcopp_inner_api_helper::splitstack_swapcontext(from_sctx, to_sctx, jump_transfer);
#endif
        res = copp::fcontext::copp_jump_fcontext(to_fctx, &jump_transfer);
        if (UTIL_CONFIG_NULLPTR == res.data) {
            abort();
            return;
        }
        jump_src = reinterpret_cast<libcopp_inner_api_helper::jump_src_data_t *>(res.data);
        assert(jump_src);

        /**
         * save from_co's fcontext and switch status
         * we should use from_co in transfer_t, because it may not jump from jump_transfer.to_co
         *
         * if we jump sequence is A->B->C->A.resume(), and if this call is A->B, then
         * jump_src->from_co = C, jump_src->to_co = A, jump_transfer.from_co = A, jump_transfer.to_co = B
         * and now we should save the callee of C and set the caller of A = C
         *
         * if we jump sequence is A->B.yield()->A, and if this call is A->B, then
         * jump_src->from_co = B, jump_src->to_co = UTIL_CONFIG_NULLPTR, jump_transfer.from_co = A, jump_transfer.to_co = B
         * and now we should save the callee of B and should change the caller of A
         *
         */

        // update caller of to_co if not jump from yield mode
        libcopp_inner_api_helper::set_caller(jump_src->to_co, res.fctx);

        libcopp_inner_api_helper::set_callee(jump_src->from_co, res.fctx);

        // private data
        jump_transfer.priv_data = jump_src->priv_data;

        // this_coroutine
        coroutine_context_base::set_this_coroutine_base(jump_transfer.from_co);
    }

    LIBCOPP_COPP_API coroutine_context::coroutine_context() LIBCOPP_MACRO_NOEXCEPT : coroutine_context_base(),
                                                                                   caller_(UTIL_CONFIG_NULLPTR),
                                                                                   callee_(UTIL_CONFIG_NULLPTR),
                                                                                   callee_stack_()
#ifdef LIBCOPP_MACRO_USE_SEGMENTED_STACKS
                                                                                   ,caller_stack_()
#endif 
    {}

    LIBCOPP_COPP_API coroutine_context::~coroutine_context() {}

    LIBCOPP_COPP_API int coroutine_context::create(coroutine_context *p, callback_t &runner, const stack_context &callee_stack,
                                                   size_t coroutine_size, size_t private_buffer_size) LIBCOPP_MACRO_NOEXCEPT {
        if (UTIL_CONFIG_NULLPTR == p) {
            return COPP_EC_ARGS_ERROR;
        }

        // must aligned to sizeof(size_t)
        if (0 != (private_buffer_size & (sizeof(size_t) - 1))) {
            return COPP_EC_ARGS_ERROR;
        }

        if (0 != (coroutine_size & (sizeof(size_t) - 1))) {
            return COPP_EC_ARGS_ERROR;
        }

        size_t stack_offset = private_buffer_size + coroutine_size;
        if (UTIL_CONFIG_NULLPTR == callee_stack.sp || callee_stack.size <= stack_offset) {
            return COPP_EC_ARGS_ERROR;
        }

        // stack down
        // |STACK BUFFER........COROUTINE..this..padding..PRIVATE DATA.....callee_stack.sp |
        // |------------------------------callee_stack.size -------------------------------|
        if (callee_stack.sp <= p || coroutine_size < sizeof(coroutine_context)) {
            return COPP_EC_ARGS_ERROR;
        }

        size_t this_offset = reinterpret_cast<unsigned char *>(callee_stack.sp) - reinterpret_cast<unsigned char *>(p);
        if (this_offset < sizeof(coroutine_context) + private_buffer_size || this_offset > stack_offset) {
            return COPP_EC_ARGS_ERROR;
        }

        // if runner is empty, we can set it later
        p->set_runner(std::move(runner));

        if (&p->callee_stack_ != &callee_stack) {
            p->callee_stack_ = callee_stack;
        }
        p->private_buffer_size_ = private_buffer_size;

        // stack down, left enough private data
        p->priv_data_ = reinterpret_cast<unsigned char *>(p->callee_stack_.sp) - p->private_buffer_size_;
        p->callee_ =
            fcontext::copp_make_fcontext(reinterpret_cast<unsigned char *>(p->callee_stack_.sp) - stack_offset,
                                         p->callee_stack_.size - stack_offset, &libcopp_inner_api_helper::coroutine_context_callback);
        if (UTIL_CONFIG_NULLPTR == p->callee_) {
            return COPP_EC_FCONTEXT_MAKE_FAILED;
        }

        return COPP_EC_SUCCESS;
    }

#if defined(LIBCOPP_MACRO_ENABLE_STD_EXCEPTION_PTR) && LIBCOPP_MACRO_ENABLE_STD_EXCEPTION_PTR
    LIBCOPP_COPP_API int coroutine_context::start(void *priv_data) {
        std::exception_ptr eptr;
        int ret = start(eptr, priv_data);
        maybe_rethrow(eptr);
        return ret;
    }

    LIBCOPP_COPP_API int coroutine_context::start(std::exception_ptr& unhandled, void *priv_data) LIBCOPP_MACRO_NOEXCEPT {
#else
    LIBCOPP_COPP_API int coroutine_context::start(void *priv_data) {
#endif
        if (UTIL_CONFIG_NULLPTR == callee_) {
            return COPP_EC_NOT_INITED;
        }

#if defined(LIBCOPP_MACRO_ENABLE_WIN_FIBER) && LIBCOPP_MACRO_ENABLE_WIN_FIBER
        {
            coroutine_context_base* this_ctx = coroutine_context_base::get_this_coroutine_base();
            if (this_ctx && this_ctx->check_flags(flag_t::EN_CFT_IS_FIBER)) {
                return copp::COPP_EC_CAN_NOT_USE_CROSS_FCONTEXT_AND_FIBER;
            }
        }
#endif

        int from_status = status_t::EN_CRS_READY;
        do {
            if (from_status < status_t::EN_CRS_READY) {
                return COPP_EC_NOT_INITED;
            }

            if (status_.compare_exchange_strong(from_status, status_t::EN_CRS_RUNNING, libcopp::util::lock::memory_order_acq_rel,
                                                libcopp::util::lock::memory_order_acquire)) {
                break;
            } else {
                // finished or stoped
                if (from_status > status_t::EN_CRS_RUNNING) {
                    return COPP_EC_NOT_READY;
                }

                // already running
                if (status_t::EN_CRS_RUNNING == from_status) {
                    return COPP_EC_IS_RUNNING;
                }
            }
        } while (true);

        jump_src_data_t jump_data;
        jump_data.from_co   = ::copp::this_coroutine::get_coroutine();
        jump_data.to_co     = this;
        jump_data.priv_data = priv_data;

#ifdef LIBCOPP_MACRO_USE_SEGMENTED_STACKS
        jump_to(callee_, caller_stack_, callee_stack_, jump_data);
#else
        jump_to(callee_, callee_stack_, callee_stack_, jump_data);
#endif

        // Move changing status to EN_CRS_EXITED is finished
        if (check_flags(flag_t::EN_CFT_FINISHED)) {
            // if in finished status, change it to exited
            status_.store(status_t::EN_CRS_EXITED, libcopp::util::lock::memory_order_release);
        }

#if defined(LIBCOPP_MACRO_ENABLE_STD_EXCEPTION_PTR) && LIBCOPP_MACRO_ENABLE_STD_EXCEPTION_PTR
        if (unlikely(unhandle_exception_)) {
            std::swap(unhandled, unhandle_exception_);
        }
#endif

        return COPP_EC_SUCCESS;
    } // namespace copp

    LIBCOPP_COPP_API int coroutine_context::resume(void *priv_data) { return start(priv_data); }
#if defined(LIBCOPP_MACRO_ENABLE_STD_EXCEPTION_PTR) && LIBCOPP_MACRO_ENABLE_STD_EXCEPTION_PTR
    LIBCOPP_COPP_API int coroutine_context::resume(std::exception_ptr& unhandled, void *priv_data) LIBCOPP_MACRO_NOEXCEPT { return start(unhandled, priv_data); }
#endif

    LIBCOPP_COPP_API int coroutine_context::yield(void **priv_data) LIBCOPP_MACRO_NOEXCEPT {
        if (UTIL_CONFIG_NULLPTR == callee_) {
            return COPP_EC_NOT_INITED;
        }

        int from_status = status_t::EN_CRS_RUNNING;
        int to_status   = status_t::EN_CRS_READY;
        if (check_flags(flag_t::EN_CFT_FINISHED)) {
            to_status = status_t::EN_CRS_FINISHED;
        }
        if (false == status_.compare_exchange_strong(from_status, to_status, libcopp::util::lock::memory_order_acq_rel,
                                                     libcopp::util::lock::memory_order_acquire)) {
            switch (from_status) {
            case status_t::EN_CRS_INVALID:
                return COPP_EC_NOT_INITED;
            case status_t::EN_CRS_READY:
                return COPP_EC_NOT_RUNNING;
            case status_t::EN_CRS_FINISHED:
            case status_t::EN_CRS_EXITED:
                return COPP_EC_ALREADY_EXIST;
            default:
                return COPP_EC_UNKNOWN;
            }
        }

        // success or finished will continue
        jump_src_data_t jump_data;
        jump_data.from_co = this;
        jump_data.to_co   = UTIL_CONFIG_NULLPTR;


#ifdef LIBCOPP_MACRO_USE_SEGMENTED_STACKS
        jump_to(caller_, callee_stack_, caller_stack_, jump_data);
#else
        jump_to(caller_, callee_stack_, callee_stack_, jump_data);
#endif

        if (UTIL_CONFIG_NULLPTR != priv_data) {
            *priv_data = jump_data.priv_data;
        }

        return COPP_EC_SUCCESS;
    }

    namespace this_coroutine {
        LIBCOPP_COPP_API int yield(void **priv_data) {
            coroutine_context *pco = get_coroutine();
            if (UTIL_CONFIG_NULLPTR != pco) {
                return pco->yield(priv_data);
            }

            return COPP_EC_NOT_RUNNING;
        }
    } // namespace this_coroutine
} // namespace copp
