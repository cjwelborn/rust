/*
  Upcalls

  These are runtime functions that the compiler knows about and generates
  calls to. They are called on the Rust stack and, in most cases, immediately
  switch to the C stack.
 */

#include "rust_cc.h"
#include "rust_internal.h"
#include "rust_task_thread.h"
#include "rust_unwind.h"
#include "rust_upcall.h"
#include "rust_util.h"
#include <stdint.h>


#ifdef __GNUC__
#define LOG_UPCALL_ENTRY(task)                            \
    LOG(task, upcall,                                     \
        "> UPCALL %s - task: %s 0x%" PRIxPTR              \
        " retpc: x%" PRIxPTR,                             \
        __FUNCTION__,                                     \
        (task)->name, (task),                             \
        __builtin_return_address(0));
#else
#define LOG_UPCALL_ENTRY(task)                            \
    LOG(task, upcall, "> UPCALL task: %s @x%" PRIxPTR,    \
        (task)->name, (task));
#endif

// This is called to ensure we've set up our rust stacks
// correctly. Strategically placed at entry to upcalls because they begin on
// the rust stack and happen frequently enough to catch most stack changes,
// including at the beginning of all landing pads.
// FIXME: Enable this for windows
#if defined __linux__ || defined __APPLE__ || defined __FreeBSD__
extern "C" void
check_stack_alignment() __attribute__ ((aligned (16)));
#else
static void check_stack_alignment() { }
#endif

#define UPCALL_SWITCH_STACK(A, F) call_upcall_on_c_stack((void*)A, (void*)F)

inline void
call_upcall_on_c_stack(void *args, void *fn_ptr) {
    check_stack_alignment();
    rust_task *task = rust_task::get_task_from_tcb();
    task->call_on_c_stack(args, fn_ptr);
}

extern "C" void record_sp_limit(void *limit);

/**********************************************************************
 * Switches to the C-stack and invokes |fn_ptr|, passing |args| as argument.
 * This is used by the C compiler to call native functions and by other
 * upcalls to switch to the C stack.  The return value is passed through a
 * field in the args parameter. This upcall is specifically for switching
 * to the shim functions generated by rustc.
 */
extern "C" CDECL void
upcall_call_shim_on_c_stack(void *args, void *fn_ptr) {
    rust_task *task = rust_task::get_task_from_tcb();

    // FIXME (1226) - The shim functions generated by rustc contain the
    // morestack prologue, so we need to let them know they have enough
    // stack.
    record_sp_limit(0);

    try {
        task->call_on_c_stack(args, fn_ptr);
    } catch (...) {
        LOG_ERR(task, task, "Native code threw an exception");
        abort();
    }

    task->record_stack_limit();
}

/*
 * The opposite of above. Starts on a C stack and switches to the Rust
 * stack. This is the only upcall that runs from the C stack.
 */
extern "C" CDECL void
upcall_call_shim_on_rust_stack(void *args, void *fn_ptr) {
    rust_task *task = rust_task_thread::get_task();

    // FIXME: Because of the hack in the other function that disables the
    // stack limit when entering the C stack, here we restore the stack limit
    // again.
    task->record_stack_limit();

    try {
        task->call_on_rust_stack(args, fn_ptr);
    } catch (...) {
        // We can't count on being able to unwind through arbitrary
        // code. Our best option is to just fail hard.
        LOG_ERR(task, task,
                "Rust task failed after reentering the Rust stack");
        abort();
    }

    // FIXME: As above
    record_sp_limit(0);
}

/**********************************************************************/

struct s_fail_args {
    char const *expr;
    char const *file;
    size_t line;
};

extern "C" CDECL void
upcall_s_fail(s_fail_args *args) {
    rust_task *task = rust_task::get_task_from_tcb();
    LOG_UPCALL_ENTRY(task);
    LOG_ERR(task, upcall, "upcall fail '%s', %s:%" PRIdPTR, 
            args->expr, args->file, args->line);
    task->fail();
}

extern "C" CDECL void
upcall_fail(char const *expr,
            char const *file,
            size_t line) {
    s_fail_args args = {expr,file,line};
    UPCALL_SWITCH_STACK(&args, upcall_s_fail);
}

/**********************************************************************
 * Allocate an object in the task-local heap.
 */

struct s_malloc_args {
    uintptr_t retval;
    type_desc *td;
};

extern "C" CDECL void
upcall_s_malloc(s_malloc_args *args) {
    rust_task *task = rust_task::get_task_from_tcb();
    LOG_UPCALL_ENTRY(task);

    LOG(task, mem, "upcall malloc(0x%" PRIxPTR ")", args->td);

    cc::maybe_cc(task);

    // FIXME--does this have to be calloc?
    rust_opaque_box *box = task->boxed.calloc(args->td);
    void *body = box_body(box);

    debug::maybe_track_origin(task, box);

    LOG(task, mem,
        "upcall malloc(0x%" PRIxPTR ") = box 0x%" PRIxPTR
        " with body 0x%" PRIxPTR,
        args->td, (uintptr_t)box, (uintptr_t)body);
    args->retval = (uintptr_t) box;
}

extern "C" CDECL uintptr_t
upcall_malloc(type_desc *td) {
    s_malloc_args args = {0, td};
    UPCALL_SWITCH_STACK(&args, upcall_s_malloc);
    return args.retval;
}

/**********************************************************************
 * Called whenever an object in the task-local heap is freed.
 */

struct s_free_args {
    void *ptr;
};

extern "C" CDECL void
upcall_s_free(s_free_args *args) {
    rust_task *task = rust_task::get_task_from_tcb();
    LOG_UPCALL_ENTRY(task);

    rust_task_thread *thread = task->thread;
    DLOG(thread, mem,
             "upcall free(0x%" PRIxPTR ", is_gc=%" PRIdPTR ")",
             (uintptr_t)args->ptr);

    debug::maybe_untrack_origin(task, args->ptr);

    rust_opaque_box *box = (rust_opaque_box*) args->ptr;
    task->boxed.free(box);
}

extern "C" CDECL void
upcall_free(void* ptr) {
    s_free_args args = {ptr};
    UPCALL_SWITCH_STACK(&args, upcall_s_free);
}

/**********************************************************************
 * Sanity checks on boxes, insert when debugging possible
 * use-after-free bugs.  See maybe_validate_box() in trans.rs.
 */

extern "C" CDECL void
upcall_validate_box(rust_opaque_box* ptr) {
    if (ptr) {
        assert(ptr->ref_count > 0);
        assert(ptr->td != NULL);
        assert(ptr->td->align <= 8);
        assert(ptr->td->size <= 4096); // might not really be true...
    }
}

/**********************************************************************
 * Allocate an object in the exchange heap.
 */

struct s_shared_malloc_args {
    uintptr_t retval;
    size_t nbytes;
};

extern "C" CDECL void
upcall_s_shared_malloc(s_shared_malloc_args *args) {
    rust_task *task = rust_task::get_task_from_tcb();
    LOG_UPCALL_ENTRY(task);

    LOG(task, mem, "upcall shared_malloc(%" PRIdPTR ")", args->nbytes);
    void *p = task->kernel->malloc(args->nbytes, "shared malloc");
    memset(p, '\0', args->nbytes);
    LOG(task, mem, "upcall shared_malloc(%" PRIdPTR ") = 0x%" PRIxPTR,
        args->nbytes, (uintptr_t)p);
    args->retval = (uintptr_t) p;
}

extern "C" CDECL uintptr_t
upcall_shared_malloc(size_t nbytes) {
    s_shared_malloc_args args = {0, nbytes};
    UPCALL_SWITCH_STACK(&args, upcall_s_shared_malloc);
    return args.retval;
}

/**********************************************************************
 * Called whenever an object in the exchange heap is freed.
 */

struct s_shared_free_args {
    void *ptr;
};

extern "C" CDECL void
upcall_s_shared_free(s_shared_free_args *args) {
    rust_task *task = rust_task::get_task_from_tcb();
    LOG_UPCALL_ENTRY(task);

    rust_task_thread *thread = task->thread;
    DLOG(thread, mem,
             "upcall shared_free(0x%" PRIxPTR")",
             (uintptr_t)args->ptr);
    task->kernel->free(args->ptr);
}

extern "C" CDECL void
upcall_shared_free(void* ptr) {
    s_shared_free_args args = {ptr};
    UPCALL_SWITCH_STACK(&args, upcall_s_shared_free);
}

struct s_shared_realloc_args {
    void *retval;
    void *ptr;
    size_t size;
};

extern "C" CDECL void
upcall_s_shared_realloc(s_shared_realloc_args *args) {
    rust_task *task = rust_task::get_task_from_tcb();
    LOG_UPCALL_ENTRY(task);
    args->retval = task->kernel->realloc(args->ptr, args->size);
}

extern "C" CDECL void *
upcall_shared_realloc(void *ptr, size_t size) {
    s_shared_realloc_args args = {NULL, ptr, size};
    UPCALL_SWITCH_STACK(&args, upcall_s_shared_realloc);
    return args.retval;
}

/**********************************************************************/

struct s_vec_grow_args {
    rust_vec** vp;
    size_t new_sz;
};

extern "C" CDECL void
upcall_s_vec_grow(s_vec_grow_args *args) {
    rust_task *task = rust_task::get_task_from_tcb();
    LOG_UPCALL_ENTRY(task);
    reserve_vec(task, args->vp, args->new_sz);
    (*args->vp)->fill = args->new_sz;
}

extern "C" CDECL void
upcall_vec_grow(rust_vec** vp, size_t new_sz) {
    s_vec_grow_args args = {vp, new_sz};
    UPCALL_SWITCH_STACK(&args, upcall_s_vec_grow);
}

struct s_str_concat_args {
    rust_vec* lhs;
    rust_vec* rhs;
    rust_vec* retval;
};

extern "C" CDECL void
upcall_s_str_concat(s_str_concat_args *args) {
    rust_vec *lhs = args->lhs;
    rust_vec *rhs = args->rhs;
    rust_task *task = rust_task::get_task_from_tcb();
    size_t fill = lhs->fill + rhs->fill - 1;
    rust_vec* v = (rust_vec*)task->kernel->malloc(fill + sizeof(rust_vec),
                                                  "str_concat");
    v->fill = v->alloc = fill;
    memmove(&v->data[0], &lhs->data[0], lhs->fill - 1);
    memmove(&v->data[lhs->fill - 1], &rhs->data[0], rhs->fill);
    args->retval = v;
}

extern "C" CDECL rust_vec*
upcall_str_concat(rust_vec* lhs, rust_vec* rhs) {
    s_str_concat_args args = {lhs, rhs, 0};
    UPCALL_SWITCH_STACK(&args, upcall_s_str_concat);
    return args.retval;
}


extern "C" _Unwind_Reason_Code
__gxx_personality_v0(int version,
                     _Unwind_Action actions,
                     uint64_t exception_class,
                     _Unwind_Exception *ue_header,
                     _Unwind_Context *context);

struct s_rust_personality_args {
    _Unwind_Reason_Code retval;
    int version;
    _Unwind_Action actions;
    uint64_t exception_class;
    _Unwind_Exception *ue_header;
    _Unwind_Context *context;
};

extern "C" void
upcall_s_rust_personality(s_rust_personality_args *args) {
    args->retval = __gxx_personality_v0(args->version,
                                        args->actions,
                                        args->exception_class,
                                        args->ue_header,
                                        args->context);
}

/**
   The exception handling personality function. It figures
   out what to do with each landing pad. Just a stack-switching
   wrapper around the C++ personality function.
*/
extern "C" _Unwind_Reason_Code
upcall_rust_personality(int version,
                        _Unwind_Action actions,
                        uint64_t exception_class,
                        _Unwind_Exception *ue_header,
                        _Unwind_Context *context) {
    s_rust_personality_args args = {(_Unwind_Reason_Code)0,
                                    version, actions, exception_class,
                                    ue_header, context};
    rust_task *task = rust_task::get_task_from_tcb();

    // The personality function is run on the stack of the
    // last function that threw or landed, which is going
    // to sometimes be the C stack. If we're on the Rust stack
    // then switch to the C stack.

    if (task->on_rust_stack()) {
        UPCALL_SWITCH_STACK(&args, upcall_s_rust_personality);
    } else {
        upcall_s_rust_personality(&args);
    }
    return args.retval;
}

extern "C" void
shape_cmp_type(int8_t *result, const type_desc *tydesc,
               const type_desc **subtydescs, uint8_t *data_0,
               uint8_t *data_1, uint8_t cmp_type);

struct s_cmp_type_args {
    int8_t *result;
    const type_desc *tydesc;
    const type_desc **subtydescs;
    uint8_t *data_0;
    uint8_t *data_1;
    uint8_t cmp_type;
};

extern "C" void
upcall_s_cmp_type(s_cmp_type_args *args) {
    shape_cmp_type(args->result, args->tydesc, args->subtydescs,
                   args->data_0, args->data_1, args->cmp_type);
}

extern "C" void
upcall_cmp_type(int8_t *result, const type_desc *tydesc,
                const type_desc **subtydescs, uint8_t *data_0,
                uint8_t *data_1, uint8_t cmp_type) {
    s_cmp_type_args args = {result, tydesc, subtydescs, data_0, data_1, cmp_type};
    UPCALL_SWITCH_STACK(&args, upcall_s_cmp_type);
}

extern "C" void
shape_log_type(const type_desc *tydesc, uint8_t *data, uint32_t level);

struct s_log_type_args {
    const type_desc *tydesc;
    uint8_t *data;
    uint32_t level;
};

extern "C" void
upcall_s_log_type(s_log_type_args *args) {
    shape_log_type(args->tydesc, args->data, args->level);
}

extern "C" void
upcall_log_type(const type_desc *tydesc, uint8_t *data, uint32_t level) {
    s_log_type_args args = {tydesc, data, level};
    UPCALL_SWITCH_STACK(&args, upcall_s_log_type);
}

struct s_new_stack_args {
    void *result;
    size_t stk_sz;
    void *args_addr;
    size_t args_sz;
};

extern "C" CDECL void
upcall_s_new_stack(struct s_new_stack_args *args) {
    rust_task *task = rust_task::get_task_from_tcb();
    args->result = task->next_stack(args->stk_sz,
                                    args->args_addr,
                                    args->args_sz);
}

extern "C" CDECL void *
upcall_new_stack(size_t stk_sz, void *args_addr, size_t args_sz) {
    s_new_stack_args args = {NULL, stk_sz, args_addr, args_sz};
    UPCALL_SWITCH_STACK(&args, upcall_s_new_stack);
    return args.result;
}

extern "C" CDECL void
upcall_s_del_stack() {
    rust_task *task = rust_task::get_task_from_tcb();
    task->prev_stack();
}

extern "C" CDECL void
upcall_del_stack() {
    UPCALL_SWITCH_STACK(NULL, upcall_s_del_stack);
}

// Landing pads need to call this to insert the
// correct limit into TLS.
// NB: This must run on the Rust stack because it
// needs to acquire the value of the stack pointer
extern "C" CDECL void
upcall_reset_stack_limit() {
    rust_task *task = rust_task_thread::get_task();
    task->reset_stack_limit();
}

//
// Local Variables:
// mode: C++
// fill-column: 78;
// indent-tabs-mode: nil
// c-basic-offset: 4
// buffer-file-coding-system: utf-8-unix
// End:
//
