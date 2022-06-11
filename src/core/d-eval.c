//
//  File: %d-eval.c
//  Summary: "Debug-Build Checks for the Evaluator"
//  Section: debug
//  Project: "Rebol 3 Interpreter and Run-time (Ren-C branch)"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012 REBOL Technologies
// Copyright 2012-2019 Ren-C Open Source Contributors
// REBOL is a trademark of REBOL Technologies
//
// See README.md and CREDITS.md for more information.
//
// Licensed under the Lesser GPL, Version 3.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// https://www.gnu.org/licenses/lgpl-3.0.html
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Due to the length of %c-eval.c and debug checks it already has, some
// debug-only routines are separated out here.  (Note that these are in
// addition to the checks already done by Push_Frame() and Drop_Frame() time)
//
// * Evaluator_Expression_Checks_Debug() runs before each full "expression"
//   is evaluated, e.g. before each EVALUATE step.  It makes sure the state
//   balanced completely--so no DS_PUSH() that wasn't balanced by a DS_DROP()
//   (for example).  It also trashes variables in the frame which might
//   accidentally carry over from one step to another, so that there will be
//   a crash instead of a casual reuse.
//
// * Evaluator_Exit_Checks_Debug() runs only if Evaluator_Executor() makes
//   it to the end without a fail() longjmping out from under it.  It also
//   checks to make sure the state has balanced, and that the return result is
//   consistent with the state being returned.
//
// Because none of these routines are in the release build, they cannot have
// any side-effects that affect the interpreter's ordinary operation.
//

#include "sys-core.h"

#undef f_value
#undef f_gotten

#define f_next f->feed->value
#define f_next_gotten f->feed->gotten

#if DEBUG_COUNT_TICKS && DEBUG_HAS_PROBE

//
//  Dump_Frame_Location: C
//
void Dump_Frame_Location(const Cell *v, REBFRM *f)
{
    DECLARE_LOCAL (dump);

    if (v) {
        Derelativize(dump, v, f_specifier);
        printf("Dump_Frame_Location() current\n");
        PROBE(dump);
    }

    if (IS_END(f_next)) {
        printf("...then Dump_Frame_Location() is at end of array\n");
        if (not v and not f_next) { // well, that wasn't informative
            if (not f->prior)
                printf("...and no parent frame, so you're out of luck\n");
            else {
                printf("...dumping parent in case that's more useful?\n");
                Dump_Frame_Location(nullptr, f->prior);
            }
        }
    }
    else {
        Derelativize(dump, f_next, f_specifier);
        printf("Dump_Frame_Location() next\n");
        PROBE(dump);

        printf("Dump_Frame_Location() rest\n");

        if (FRM_IS_VARIADIC(f)) {
            //
            // NOTE: This reifies the va_list in the frame, which should not
            // affect procssing.  But it is a side-effect and may need to be
            // avoided if the problem you are debugging was specifically
            // related to va_list frame processing.
            //
            const bool truncated = true;
            Reify_Va_To_Array_In_Feed(f->feed, truncated);
        }

        Init_Any_Array_At_Core(
            dump,
            REB_BLOCK,
            f_array,
            cast(REBLEN, f_index),
            f_specifier
        );
        PROBE(dump);
    }
}

#endif


#if !defined(NDEBUG)

// These are checks common to Expression and Exit checks (hence also common
// to the "end of Start" checks, since that runs on the first expression)
//
static void Evaluator_Shared_Checks_Debug(REBFRM *f)
{
    // The state isn't actually guaranteed to balance overall until a frame
    // is completely dropped.  This is because a frame may be reused over
    // multiple calls by something like REDUCE or FORM, accumulating items
    // on the data stack or mold stack/etc.  See Drop_Frame() for the actual
    // balance check.
    //
    ASSERT_NO_DATA_STACK_POINTERS_EXTANT();

    // See notes on f->feed->gotten about the coherence issues in the face
    // of arbitrary function execution.
    //
    if (f_next_gotten) {
        assert(IS_WORD(f_next));
        assert(Lookup_Word(f_next, f_specifier) == f_next_gotten);
    }

    assert(f == FS_TOP);

    // If this fires, it means that Flip_Series_To_White was not called an
    // equal number of times after Flip_Series_To_Black, which means that
    // the custom marker on series accumulated.
    //
    assert(TG_Num_Black_Series == 0);

    // We only have a label if we are in the middle of running a function,
    // and if we're not running a function then f->original should be null.
    //
    assert(not f->original);
    assert(IS_POINTER_TRASH_DEBUG(unwrap(f->label)));

    if (f->varlist) {
        assert(NOT_SERIES_FLAG(f->varlist, MANAGED));
        assert(NOT_SERIES_FLAG(f->varlist, INACCESSIBLE));
    }

    assert(NOT_FEED_FLAG(f->feed, NEXT_ARG_FROM_OUT));  // new expression

    //=//// ^-- ABOVE CHECKS *ALWAYS* APPLY ///////////////////////////////=//

    if (IS_END(f_next))
        return;

    if (Is_Throwing(f))
        return;

    //=//// v-- BELOW CHECKS ONLY APPLY IN EXITS CASE WITH MORE CODE //////=//

    assert(NOT_END(f_next));
    assert(f_next != f->out);

    //=//// ^-- ADD CHECKS EARLIER THAN HERE IF THEY SHOULD ALWAYS RUN ////=//
}


//
//  Evaluator_Expression_Checks_Debug: C
//
// These fields are required upon initialization:
//
//     f->out
//     REBVAL pointer to which the evaluation's result should be written.
//     Should be to writable memory in a cell that lives above this call to
//     the evalutor in stable memory (not user-visible, e.g. DECLARE_LOCAL
//     or the parent's f->spare).  This can't point into an array whose memory
//     may move during arbitrary evaluation, and that includes cells on the
//     expandable data stack.  It also usually can't write a function argument
//     cell, because that could expose an unfinished calculation during this
//     Action_Executor() through its FRAME!...though an Action_Executor(f) must
//     write f's *own* arg slots to fulfill them.
//
//     f->feed
//     Contains the REBARR* or C va_list of subsequent values to fetch...as
//     well as the specifier.  The current value, its cached "gotten" value if
//     it is a WORD!, and other information is stored here through a level of
//     indirection so it may be shared and updated between recursions.
//
// This routine attempts to "trash" a lot of frame state variables to help
// make sure one evaluation does not leak data into the next.
//
void Evaluator_Expression_Checks_Debug(REBFRM *f)
{
    assert(f == FS_TOP); // should be topmost frame, still

    assert(NOT_EVAL_FLAG(f, DIDNT_LEFT_QUOTE_PATH));
    if (NOT_EVAL_FLAG(f, FULFILLING_ARG))
        assert(NOT_FEED_FLAG(f->feed, NO_LOOKAHEAD));
    assert(NOT_FEED_FLAG(f->feed, DEFERRING_ENFIX));

    Evaluator_Shared_Checks_Debug(f);

    assert(not Is_Throwing(f)); // no evals between throws

    // Trash fields that GC won't be seeing unless Is_Action_Frame()
    //
    TRASH_POINTER_IF_DEBUG(f->key);
    TRASH_POINTER_IF_DEBUG(f->arg);
    TRASH_POINTER_IF_DEBUG(f->param);

    assert(not f->varlist or NOT_SERIES_FLAG(f->varlist, INACCESSIBLE));

    // Mutate va_list sources into arrays at fairly random moments in the
    // debug build.  It should be able to handle it at any time.
    //
    if (FRM_IS_VARIADIC(f) and SPORADICALLY(50)) {
        const bool truncated = true;
        Reify_Va_To_Array_In_Feed(f->feed, truncated);
    }
}


//
//  Do_After_Action_Checks_Debug: C
//
void Do_After_Action_Checks_Debug(REBFRM *f) {
    assert(not Is_Throwing(f));

    if (GET_SERIES_FLAG(f->varlist, INACCESSIBLE))  // e.g. ENCLOSE
        return;

    REBACT *phase = FRM_PHASE(f);

    // Usermode functions check the return type via Func_Dispatcher(),
    // with everything else assumed to return the correct type.  But this
    // double checks any function marked with RETURN in the debug build,
    // so native return types are checked instead of just trusting the C.
    //
    // !!! PG_Dispatcher() should do this, so every phase gets checked.
    //
  #if DEBUG_NATIVE_RETURNS
    if (ACT_HAS_RETURN(phase)) {
        const REBKEY *key = ACT_KEYS_HEAD(phase);
        const REBPAR *param = ACT_PARAMS_HEAD(phase);
        assert(KEY_SYM(key) == SYM_RETURN);

        if (Is_Stale(f->out) or Is_Void(f->out)) {
            //
            // If a function is void, it left whatever was in the output
            // from before it ran.  So there's no correspondence to the return
            // types it declares it could return itself (if the output was
            // not flagged with the stale bit).
            //
            // Doesn't make sense to type check some arbitrary other function's
            // return result we are passing through!

            if (NOT_PARAM_FLAG(param, VANISHABLE)) {
                Clear_Stale_Flag(f->out);  // let VAL_TYPE() work
                assert(!"Native code violated return type contract!\n");
                panic (Error_Bad_Return_Type(
                    f,
                    Is_Void(f->out) ? REB_0_VOID : VAL_TYPE(f->out)
                ));
            }
        }
        else if (
            not Typecheck_Including_Constraints(param, f->out)
            and not (
                GET_PARAM_FLAG(param, VANISHABLE)
                and GET_EVAL_FLAG(f, RUNNING_ENFIX)
            )  // exemption, e.g. `1 comment "hi" + 2` infix non-stale
        ){
            assert(!"Native code violated return type contract!\n");
            panic (Error_Bad_Return_Type(
                f,
                Is_Void(f->out) ? REB_0_VOID : VAL_TYPE(f->out)
            ));
        }
    }
  #endif
}


//
//  Evaluator_Exit_Checks_Debug: C
//
void Evaluator_Exit_Checks_Debug(REBFRM *f) {
    Evaluator_Shared_Checks_Debug(f);

    if (NOT_END(f_next) and not FRM_IS_VARIADIC(f)) {
        if (f_index > ARR_LEN(f_array)) {
            assert(Is_Throwing(f));
            assert(f_index == ARR_LEN(f_array) + 1);
        }
    }

  //=//// CHECK FOR STRAY FLAGS ///////////////////////////////////////////=//

    if (not Is_Throwing(f)) {
        REBFLGS filtered = (f->flags.bits & ~FLAG_STATE_BYTE(255));
        filtered &= ~ (
            EVAL_FLAG_0_IS_TRUE  // always true
            | EVAL_FLAG_7_IS_TRUE  // always true
            | EVAL_FLAG_ALLOCATED_FEED
            | EVAL_FLAG_ROOT_FRAME
            | EVAL_FLAG_TRAMPOLINE_KEEPALIVE
        );

        // These are provided as options to Evaluator_Executor, and should not
        // change over the course of the evaluation (could check this?)  But in
        // any case they are okay if they are set.
        //
        filtered &= ~ (
            EVAL_FLAG_MAYBE_STALE
            | EVAL_FLAG_TO_END
            | EVAL_FLAG_BRANCH
            | EVAL_FLAG_META_RESULT
            | EVAL_FLAG_FULFILLING_ARG
            | EVAL_FLAG_NO_RESIDUE
        );

        if (filtered != 0) {
            int bit;
            for (bit = 0; bit < 32; ++bit)
                if (filtered & FLAG_LEFT_BIT(bit))
                    printf("BIT %d SET in EVAL_FLAGS\n", bit);

            assert(!"Unexpected stray flags found in evaluator finalization");
        }
    }
}

#endif
