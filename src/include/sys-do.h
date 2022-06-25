//
//  File: %sys-do.h
//  Summary: {DO-until-end (of block or variadic feed) evaluation API}
//  Project: "Rebol 3 Interpreter and Run-time (Ren-C branch)"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012 REBOL Technologies
// Copyright 2012-2019 Ren-C Open Source Contributors
// REBOL is a trademark of REBOL Technologies
//
// See README.md and CREDITS.md for more information
//
// Licensed under the Lesser GPL, Version 3.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// https://www.gnu.org/licenses/lgpl-3.0.html
//
//=////////////////////////////////////////////////////////////////////////=//
//
// The "DO" helpers have names like Do_XXX(), and are a convenience layer
// over making repeated calls into the Eval_XXX() routines.  DO-ing things
// always implies running to the end of an input.  It also implies returning
// a BAD-WORD! value if nothing can be synthesized, but letting the last null
// or value fall out otherwise:
//
//     >> type of ^ do []
//     == bad-word!
//
//     >> type of ^ do [comment "hi"]
//     == bad-word!
//
//     >> do [1 comment "hi"]
//     == 1
//
//    >> do [null comment "hi"]
//    ; null
//
// See %sys-eval.h for the lower level routines if this isn't enough control.
//
//=//// NOTES //////////////////////////////////////////////////////////////=//
//
// * Unlike single stepping, the stale flag from Do_XXX_Maybe_Stale() isn't
//   generally all that useful.  That's because heeding the stale flag after
//   multiple steps usually doesn't make any real sense.  If someone writes:
//
//        (1 + 2 if true [x] else [y] comment "hello")
//
//   ...what kind of actionability is there on the fact that the last step
//   vanished, if that's the only think you know?  For this reason, you'll
//   get an assert if you preload a frame with any values unless you use
//   the EVAL_FLAG_MAYBE_STALE option on the frame.
//


#define rebRunThrows(out,...) \
    rebRunCoreThrows( \
        RESET(out), \
        EVAL_MASK_DEFAULT | EVAL_FLAG_NO_RESIDUE | EVAL_FLAG_SINGLE_STEP, \
        __VA_ARGS__ \
    )


// (Used by DO and EVALUATE)
//
// If `source` is not const, tweak it to be explicitly mutable--because
// otherwise, it would wind up inheriting the FEED_MASK_CONST of our
// currently executing frame.  That's no good for `repeat 2 [do block]`,
// because we want whatever constness is on block...
//
// (Note we *can't* tweak values that are Cell in source.  So we either
// bias to having to do this or Do_XXX() versions explode into passing
// mutability parameters all over the place.  This is better.)
//
inline static void Tweak_Non_Const_To_Explicitly_Mutable(Value *source) {
    if (Not_Cell_Flag(source, CONST))
        Set_Cell_Flag(source, EXPLICITLY_MUTABLE);
}


// This helper routine is able to take an arbitrary input cell to start with
// that may not be END.  It is code that DO shares with GROUP! evaluation
// in Eval_Core()--where being able to know if a group "completely vaporized"
// is important as distinct from an expression evaluating to void.
//
inline static bool Do_Feed_To_End_Throws(
    REBVAL *out,  // must be initialized, unchanged if all empty/invisible
    REBFED *feed,  // feed mechanics always call va_end() if va_list
    REBFLGS flags
){
    // You can feed in something other than END here (and GROUP! handling in
    // the evaluator does do that).  But if you give it something stale then
    // that suggests you might be thinking you can infer some information
    // about the staleness after the run.  See comments at top of file for
    // why that's not the case--this assert helps avoid misunderstandings.
    //
    if (not (flags & EVAL_FLAG_MAYBE_STALE))
        assert(Is_Fresh(out));

    assert(not (flags & EVAL_FLAG_SINGLE_STEP));

    DECLARE_FRAME (
        f,
        feed,
        (flags
            | EVAL_FLAG_MAYBE_STALE
        ) & (~ EVAL_FLAG_BRANCH)
    );

    Push_Frame(out, f);

    do {
        if (Trampoline_Throws(f)) {
            Abort_Frame(f);
            return true;
        }
    } while (NOT_END(feed->value));

    Drop_Frame(f);

    if (not (flags & EVAL_FLAG_MAYBE_STALE))
        Clear_Stale_Flag(out);

    if (flags & EVAL_FLAG_BRANCH)
        Reify_Branch_Out(out);

    return false;
}

inline static bool Do_Any_Array_At_Core_Throws(
    REBVAL *out,
    REBFLGS flags,
    const Cell *any_array,
    REBSPC *specifier
){
    DECLARE_FEED_AT_CORE (feed, any_array, specifier);

    bool threw = Do_Feed_To_End_Throws(
        out,
        feed,
        flags | EVAL_FLAG_ALLOCATED_FEED
    );

    return threw;
}

#define Do_Any_Array_At_Throws(out,any_array,specifier) \
    Do_Any_Array_At_Core_Throws(RESET(out), EVAL_MASK_DEFAULT, (any_array), \
        (specifier))


// !!! When working with an array outside of the context of a REBVAL it was
// extracted from, then that means automatic determination of the CONST rules
// isn't possible.  This primitive is currently used in a few places where
// the desire is not to inherit any "wave of constness" from the parent's
// frame, or from a value.  The cases need review--in particular the use for
// the kind of shady frame translations used by HIJACK and ports.
//
inline static bool Do_At_Mutable_Maybe_Stale_Throws(
    REBVAL *out,
    option(const Cell*) first,  // element to inject *before* the array
    REBARR *array,
    REBLEN index,
    REBSPC *specifier  // must match array, but also first if relative
){
    // need to pass `first` parameter, so can't use DECLARE_ARRAY_FEED
    REBFED *feed = Alloc_Feed();  // need `first`
    Prep_Array_Feed(
        feed,
        first,
        array,
        index,
        specifier,
        FEED_MASK_DEFAULT  // different: does not
    );

    return Do_Feed_To_End_Throws(
        out,
        feed,
        EVAL_MASK_DEFAULT | EVAL_FLAG_ALLOCATED_FEED
            | EVAL_FLAG_MAYBE_STALE  // !!! Used for HIJACK, but always?
    );
}


inline static bool Do_Branch_Throws(
    REBVAL *out,
    const REBVAL *branch,
    const REBVAL *with
){
    if (not Pushed_Continuation(out, EVAL_FLAG_BRANCH, branch, SPECIFIED, with))
        return false;

    if (Trampoline_Throws(FS_TOP)) {
        Abort_Frame(FS_TOP);
        return true;
    }
    Drop_Frame(FS_TOP);
    return false;
}


inline static REB_R Run_Generic_Dispatch_Core(
    const REBVAL *first_arg,  // !!! Is this always same as FRM_ARG(f, 1)?
    REBFRM *f,
    const Symbol *verb
){
    GENERIC_HOOK *hook = IS_QUOTED(first_arg)
        ? &T_Quoted  // a few things like COPY are supported by QUOTED!
        : Generic_Hook_For_Type_Of(first_arg);

    REB_R r = hook(f, verb);  // Note that QUOTED! has its own hook & handling
    if (r == R_UNHANDLED)  // convenience for error handling
        fail (Error_Cannot_Use(verb, first_arg));

    return r;
}


// Some routines invoke Run_Generic_Dispatch(), go ahead and reduce the
// cases they have to look at by moving any ordinary outputs into f->out, and
// make throwing the only exceptional case they have to handle.
//
inline static bool Run_Generic_Dispatch_Throws(
    const REBVAL *first_arg,  // !!! Is this always same as FRM_ARG(f, 1)?
    REBFRM *f,
    const Symbol *verb
){
    REB_R r = Run_Generic_Dispatch_Core(first_arg, f, verb);

    if (r == f->out) {
         // common case
    }
    else if (r == nullptr) {
        Init_Nulled(f->out);
    }
    else if (IS_RETURN_SIGNAL(r)) {
        if (r == R_THROWN)
            return true;
        assert(!"Unhandled return signal from Run_Generic_Dispatch_Core");
    }
    else {
        assert(not Is_Stale(r));
        assert(Is_Api_Value(r));
        Copy_Cell(f->out, r);
        Release_Api_Value_If_Unmanaged(r);
    }
    return false;
}
