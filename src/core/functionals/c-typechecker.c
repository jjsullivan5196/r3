//
//  File: %c-typechecker.c
//  Summary: "Function generator for an optimized typechecker"
//  Section: datatypes
//  Project: "Ren-C Language Interpreter and Run-time Environment"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2016-2022 Ren-C Open Source Contributors
//
// See README.md and CREDITS.md for more information.
//
// Licensed under the GNU Lesser General Public License (LGPL), Version 3.0.
// You may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// https://www.gnu.org/licenses/lgpl-3.0.en.html
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Making a typechecker can be easy:
//
//     >> integer?: func [v [any-value!]] [integer! = kind of :v]
//
//     >> integer? 10
//     == ~true~  ; isotope
//
//     >> integer? <foo>
//     == ~false~  ; isotope
//
// But given that it is done so often, it's more efficient to have a custom
// dispatcher for making a typechecker:
//
//     >> integer?: typechecker &integer
//
// This makes a near-native optimized version of the type checker, that
// leverages the "Intrinsic" facility...so that the evaluator and type checking
// can call the C implementation directly without building a frame for the
// ACTION! call.
//

#include "sys-core.h"

enum {
    IDX_TYPECHECKER_CFUNC = IDX_INTRINSIC_CFUNC,  // uses Intrinsic_Dispatcher()
    IDX_TYPECHECKER_TYPE,  // datatype or typeset to check
    IDX_TYPECHECKER_MAX
};


//
//  Datatype_Checker_Intrinsic: C
//
// Intrinsic used by TYPECHECKER generator for when argument is a datatype.
//
void Datatype_Checker_Intrinsic(Value(*) out, Phase* phase, Value(*) arg)
{
    assert(ACT_DISPATCHER(phase) == &Intrinsic_Dispatcher);

    Details* details = Phase_Details(phase);
    assert(Array_Len(details) == IDX_TYPECHECKER_MAX);

    REBVAL *datatype = DETAILS_AT(details, IDX_TYPECHECKER_TYPE);

    Init_Logic(out, VAL_TYPE(arg) == VAL_TYPE_KIND(datatype));
}


//
//  Typeset_Checker_Intrinsic: C
//
// Intrinsic used by TYPECHECKER generator for when argument is a typeset.
//
void Typeset_Checker_Intrinsic(Value(*) out, Phase* phase, Value(*) arg)
{
    assert(ACT_DISPATCHER(phase) == &Intrinsic_Dispatcher);

    Details* details = Phase_Details(phase);
    assert(Array_Len(details) == IDX_TYPECHECKER_MAX);

    REBVAL *typeset_index = DETAILS_AT(details, IDX_TYPECHECKER_TYPE);
    assert(IS_INTEGER(typeset_index));
    Index n = VAL_INT32(typeset_index);

    REBU64 typeset = Typesets[n];
    enum Reb_Kind kind = VAL_TYPE(arg);
    Init_Logic(out, FLAGIT_KIND(kind) & typeset);
}


//
//  Make_Typechecker: C
//
// Bootstrap creates typechecker functions before functions like TYPECHECKER
// are allowed to run to create them.  So this is factored out.
//
Phase* Make_Typechecker(Value(const*) type) {
    assert(
        IS_TYPE_WORD(type)  // datatype
        or IS_INTEGER(type)  // typeset index (for finding bitset)
    );

    // We need a spec for our typecheckers, which is really just `value`
    // with no type restrictions.
    //
    DECLARE_STABLE (spec);
    Array* spec_array = Alloc_Singular(NODE_FLAG_MANAGED);
    Init_Word(Array_Single(spec_array), Canon(VALUE));
    Init_Block(spec, spec_array);

    Context* meta;
    Flags flags = MKF_KEYWORDS | MKF_RETURN;
    Array* paramlist = Make_Paramlist_Managed_May_Fail(
        &meta,
        spec,
        &flags  // return type checked only in debug build
    );
    Assert_Series_Term_If_Needed(paramlist);

    Phase* typechecker = Make_Action(
        paramlist,
        nullptr,  // no partials
        &Intrinsic_Dispatcher,  // leverage Intrinsic's optimized calls
        IDX_TYPECHECKER_MAX  // details array capacity
    );

    Details* details = Phase_Details(typechecker);

    Init_Handle_Cfunc(
        DETAILS_AT(details, IDX_TYPECHECKER_CFUNC),
        IS_TYPE_WORD(type)
            ? cast(CFunction*, &Datatype_Checker_Intrinsic)
            : cast(CFunction*, &Typeset_Checker_Intrinsic)
    );
    Copy_Cell(DETAILS_AT(details, IDX_TYPECHECKER_TYPE), type);

    return typechecker;
}


//
//  typechecker: native [
//
//  {Generator for an optimized typechecking ACTION!}
//
//      return: [activation?]
//      type [type-word! integer!]
//  ]
//
DECLARE_NATIVE(typechecker)
{
    INCLUDE_PARAMS_OF_TYPECHECKER;

    Phase* typechecker = Make_Typechecker(ARG(type));
    return Init_Activation(OUT, typechecker, ANONYMOUS, UNBOUND);
}


//
//  Typecheck_Value: C
//
// Ren-C has eliminated the concept of TYPESET!, instead gaining behaviors
// for TYPE-BLOCK! and TYPE-GROUP!.
//
bool Typecheck_Value(
    const Cell* tests,  // can be BLOCK!, TYPE-BLOCK!, GROUP!, TYPE-GROUP!
    Specifier* tests_specifier,
    Atom(const*) v
){
    DECLARE_LOCAL (spare);  // !!! stackful

    const Cell* tail;
    const Cell* item;
    bool match_all;

    switch (VAL_TYPE(tests)) {
      case REB_BLOCK:
      case REB_TYPE_BLOCK:
        item = VAL_ARRAY_AT(&tail, tests);
        match_all = false;
        break;

      case REB_GROUP:
      case REB_TYPE_GROUP:
        item = VAL_ARRAY_AT(&tail, tests);
        match_all = true;
        break;

      case REB_PARAMETER: {
        const Array* array = try_unwrap(VAL_PARAMETER_ARRAY(tests));
        if (array == nullptr)
            return true;  // implicitly all is permitted
        item = Array_Head(array);
        tail = Array_Tail(array);
        match_all = false;
        break; }

      case REB_TYPE_WORD:
        item = tests;
        tail = tests + 1;
        match_all = true;
        break;

      default:
        assert(false);
        fail ("Bad test passed to Typecheck_Value");
    }

    for (; item != tail; ++item) {
        ASSERT_CELL_READABLE_EVIL_MACRO(item);

        Option(const Symbol*) label = nullptr;  // so goto doesn't cross

        // !!! Ultimately, we'll enable literal comparison for quoted/quasi
        // items.  For the moment just try quasi-words for isotopes.
        //
        if (VAL_TYPE_UNCHECKED(item) == REB_QUASI) {
            if (HEART_BYTE(item) == REB_VOID) {
                if (Is_None(v))
                    goto test_succeeded;
                goto test_failed;
            }

            if (HEART_BYTE(item) != REB_WORD)
                fail (item);

            if (not Is_Isoword(v))
                continue;
            if (VAL_WORD_SYMBOL(v) == VAL_WORD_SYMBOL(item))
                goto test_succeeded;
            goto test_failed;
        }

        enum Reb_Kind kind;
        const Cell* test;
        if (VAL_TYPE_UNCHECKED(item) == REB_WORD) {
            label = VAL_WORD_SYMBOL(item);
            test = Lookup_Word_May_Fail(item, tests_specifier);
            kind = VAL_TYPE(test);  // e.g. TYPE-BLOCK! <> BLOCK!
        }
        else {
            test = item;
            switch (VAL_TYPE_UNCHECKED(test)) {
              case REB_BLOCK:
                kind = REB_TYPE_BLOCK;
                break;

              case REB_GROUP:
                kind = REB_TYPE_GROUP;
                break;

              default:
                kind = VAL_TYPE_UNCHECKED(test);
                break;
            }
        }

        if (Is_Activation(test))
            goto run_activation;

        switch (kind) {
          run_activation: {
            Action* action = VAL_ACTION(test);

            if (ACT_DISPATCHER(action) == &Intrinsic_Dispatcher) {
                assert(IS_DETAILS(action));
                Intrinsic* intrinsic = Extract_Intrinsic(cast(Phase*, action));

                Param* param = ACT_PARAM(action, 2);
                DECLARE_LOCAL (arg);
                Copy_Cell(arg, v);
                if (VAL_PARAM_CLASS(param) == PARAM_CLASS_META)
                    Meta_Quotify(arg);
                if (not Typecheck_Coerce_Argument(param, arg))
                    goto test_failed;

                DECLARE_LOCAL (out);
                (*intrinsic)(out, cast(Phase*, action), Stable_Unchecked(arg));
                if (not IS_LOGIC(out))
                    fail (Error_No_Logic_Typecheck(label));
                if (VAL_LOGIC(out))
                    goto test_succeeded;
                goto test_failed;
            }

            Flags flags = 0;
            Level* L = Make_End_Level(
                FLAG_STATE_BYTE(ST_ACTION_TYPECHECKING) | flags
            );
            Push_Action(L, VAL_ACTION(test), VAL_FRAME_BINDING(test));
            Begin_Prefix_Action(L, VAL_FRAME_LABEL(test));

            const Key* key = L->u.action.key;
            const Param* param = L->u.action.param;
            Atom(*) arg = L->u.action.arg;
            for (; key != L->u.action.key_tail; ++key, ++param, ++arg) {
                if (Is_Specialized(param))
                    Copy_Cell(arg, param);
                else
                    Finalize_None(arg);
                assert(Is_Stable(arg));
            }

            arg = First_Unspecialized_Arg(&param, L);
            if (not arg)
                fail (Error_No_Arg_Typecheck(label));  // must take argument

            Copy_Cell(arg, v);  // do not decay [4]

            if (VAL_PARAM_CLASS(param) == PARAM_CLASS_META)
                Meta_Quotify(arg);

            if (not Typecheck_Coerce_Argument(param, arg)) {
                Drop_Action(L);
                if (match_all)
                    return false;
                continue;
            }

            Push_Level(spare, L);

            if (Trampoline_With_Top_As_Root_Throws())
                fail (Error_No_Catch_For_Throw(TOP_LEVEL));

            Drop_Level(L);

            if (not IS_LOGIC(spare))
                fail (Error_No_Logic_Typecheck(label));

            if (not VAL_LOGIC(spare))
                goto test_failed;
            break; }

          case REB_TYPE_BLOCK:
          case REB_TYPE_GROUP: {
            Specifier* subspecifier = Derive_Specifier(tests_specifier, test);
            if (not Typecheck_Value(test, subspecifier, v))
                goto test_failed;
            break; }

          case REB_QUOTED:
          case REB_QUASI: {
            fail ("QUOTED! and QUASI! not currently supported in TYPE-XXX!"); }

          case REB_PARAMETER: {
            if (not Typecheck_Value(test, SPECIFIED, v))
                goto test_failed;
            break; }

          case REB_TYPE_WORD: {
            enum Reb_Kind k;
            if (Is_Isotope(v) and Is_Isotope_Unstable(v))
                k = REB_ISOTOPE;
            else
                k = VAL_TYPE(v);
            if (VAL_TYPE_KIND(test) != k)
                goto test_failed;
            break; }

          case REB_TAG: {
            bool strict = false;

            if (0 == CT_String(test, Root_Opt_Tag, strict)) {
                if (not Is_Nulled(v))
                    goto test_failed;
            }
            if (0 == CT_String(test, Root_Void_Tag, strict)) {
                if (not Is_Void(v))
                    goto test_failed;
            }
            break; }  // currently, ignore all other tags

          default:
            fail ("Invalid element in TYPE-GROUP!");
        }
        goto test_succeeded;

      test_succeeded:
        if (not match_all)
            return true;
        continue;

      test_failed:
        if (match_all)
            return false;
        continue;
    }

    if (match_all)
        return true;
    return false;
}


//
//  Typecheck_Coerce_Argument: C
//
// This does extra typechecking pertinent to function parameters, compared to
// the basic type checking.
//
// 1. !!! Should explicit mutability override, so people can say things
//    like `foo: func [...] mutable [...]` ?  This seems bad, because the
//    contract of the function hasn't been "tweaked" with reskinning.
//
bool Typecheck_Coerce_Argument(
    const Param* param,
    Atom(*) arg  // need mutability for coercion
){
    if (GET_PARAM_FLAG(param, CONST))
        Set_Cell_Flag(arg, CONST);  // mutability override?  [1]

    if (
        GET_PARAM_FLAG(param, REFINEMENT)
        or GET_PARAM_FLAG(param, SKIPPABLE)
    ){
        if (Is_Nulled(arg))  // nulls always legal...means refinement not used
            return true;

        if (Is_Parameter_Unconstrained(param))  // no-arg refinement
            return Is_Blackhole(arg);  // !!! Error_Bad_Argless_Refine(key)
    }

    bool coerced = false;

    // We do an adjustment of the argument to accommodate meta parameters,
    // which check the unquoted type.
    //
    bool unquoted = false;

    if (VAL_PARAM_CLASS(param) == PARAM_CLASS_META) {
        if (Is_Nulled(arg))
            return GET_PARAM_FLAG(param, ENDABLE);

        if (not Is_Quasi(arg) and not Is_Quoted(arg))
            return false;

        Meta_Unquotify_Undecayed(arg);  // temporary adjustment (easiest option)
        unquoted = true;
    }
    else if (VAL_PARAM_CLASS(param) == PARAM_CLASS_RETURN) {
        unquoted = false;
    }
    else {
        unquoted = false;

        if (not Is_Stable(arg))
            goto do_coercion;
    }

  typecheck_again:

    if (TYPE_CHECK(param, arg))
        goto return_true;

    if (not coerced) {

      do_coercion:

        if (Is_Activation(arg)) {
            QUOTE_BYTE(arg) = UNQUOTED_1;
            coerced = true;
            goto typecheck_again;
        }

        if (Is_Raised(arg))
            goto return_false;

        if (Is_Pack(arg) and Is_Pack_Undecayable(arg))
            goto return_false;  // nihil or unstable isotope in first slot

        if (Is_Barrier(arg))
            goto return_false;  // comma isotopes

        if (Is_Isotope(arg) and Is_Isotope_Unstable(arg)) {
            Decay_If_Unstable(arg);
            coerced = true;
            goto typecheck_again;
        }
    }

  return_false:

    if (unquoted)
        Meta_Quotify(arg);

    return false;

  return_true:

    if (unquoted)
        Meta_Quotify(arg);

    if (not Is_Stable(arg))
        assert(VAL_PARAM_CLASS(param) == PARAM_CLASS_RETURN);

    return true;
}
