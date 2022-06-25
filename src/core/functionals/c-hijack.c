//
//  File: %c-hijack.c
//  Summary: "Method for intercepting one function invocation with another"
//  Section: datatypes
//  Project: "Ren-C Language Interpreter and Run-time Environment"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2016-2021 Ren-C Open Source Contributors
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
// HIJACK is a tricky-but-useful mechanism for replacing calls to one function
// with another function, based on identity.  This is distinct from overwriting
// a variable, because all references are affected:
//
//     >> victim: func [] [print "This gets hijacked."]
//
//     >> reference: :victim  ; both words point to the same function identity
//
//     >> victim
//     This gets hijacked.
//
//     >> reference
//     This gets hijacked.
//
//     >> hijack :victim (func [] [print "HIJACK!"])
//
//     >> victim
//     HIJACK!
//
//     >> reference
//     HIJACK!
//
// Though it originated as a somewhat hacky experiment, it was solidified as
// it became increasingly leaned on for important demos.  HIJACK is now
// considered to be safe for mezzanine usages (where appropriate).
//
//=//// NOTES //////////////////////////////////////////////////////////////=//
//
// * Specializations, adaptations, enclosures, or other compositional tools
//   hold "references" to functions internally.  These references are also
//   affected by the hijacking, which means it's easy to get infinite loops:
//
//       >> hijack :load (adapt :load [print "LOADING!"])
//
//       >> load "<for example>"
//       LOADING!
//       LOADING!
//       LOADING!  ; ... infinite loop
//
//   The problem there is that the adaptation performs its printout and then
//   falls through to the original LOAD, which is now the hijacked version
//   that has the adaptation.  Working around this problem means remembering
//   to ADAPT a COPY:
//
//       >> hijack :load (adapt copy :load [print "LOADING!"])
//
//       >> load "<for example>"
//       LOADING!
//       == [<for example>]
//
// * Hijacking is only efficient when the frames of the functions match--e.g.
//   when the "hijacker" is an ADAPT or ENCLOSE of a copy of the "victim".  But
//   if the frames don't line up, there's an attempt to remap the parameters in
//   the frame based on their name.  This should be avoided if possible.
//

#include "sys-core.h"


//
//  Push_Redo_Action_Frame: C
//
// This code takes a running call frame that has been built for one action
// and then tries to map its parameters to invoke another action.  The new
// action may have different orders and names of parameters.
//
// R3-Alpha had a rather brittle implementation, that had no error checking
// and repetition of logic in Eval_Core.  Because R3-Alpha refinements took
// multiple arguments, it could also fail with "adversarial" prototypes:
//
//     foo: func [a /b c] [...]  =>  bar: func [/b d e] [...]
//                    foo/b 1 2  =>  bar/b 1 2
//
void Push_Redo_Action_Frame(REBVAL *out, REBFRM *f1, const REBVAL *run)
{
    REBARR *normals = Make_Array(FRM_NUM_ARGS(f1));  // max, e.g. no refines

    REBDSP dsp_orig = DSP;  // we push refinements as we find them

    EVARS e;  // use EVARS to get parameter reordering right (in theory?)
    Init_Evars(&e, CTX_ARCHETYPE(Context_For_Frame_May_Manage(f1)));

    while (Did_Advance_Evars(&e)) {
        if (Is_Specialized(e.param))  // specialized or local
            continue;

        if (VAL_PARAM_CLASS(e.param) == PARAM_CLASS_RETURN)
            continue;  // !!! hack, has PARAM_FLAG_REFINEMENT, don't stack it

        if (GET_PARAM_FLAG(e.param, SKIPPABLE) and Is_Nulled(e.var))
            continue;  // don't throw in skippable args that are nulled out

        if (GET_PARAM_FLAG(e.param, REFINEMENT)) {
            if (Is_Nulled(e.var))  // don't add to PATH!
                continue;

            Init_Word(DS_PUSH(), KEY_SYMBOL(e.key));

            if (Is_Typeset_Empty(e.param)) {
                assert(Is_Blackhole(e.var));  // used but argless refinement
                continue;
            }
        }

        // The arguments were already evaluated to put them in the frame, do
        // not evaluate them again.
        //
        // !!! This tampers with the VALUE_FLAG_UNEVALUATED bit, which is
        // another good reason this should probably be done another way.  It
        // also loses information about the const bit.
        //
        Quotify(Append_Value(normals, e.var), 1);
    }

    Shutdown_Evars(&e);

    DECLARE_LOCAL (block);
    Init_Block(block, normals);
    DECLARE_FRAME_AT (f2, block, EVAL_MASK_DEFAULT | EVAL_FLAG_MAYBE_STALE);
    f2->baseline.dsp = dsp_orig;

    Push_Frame(out, f2);
    Push_Action(f2, VAL_ACTION(run), VAL_ACTION_BINDING(run));
    Begin_Prefix_Action(f2, VAL_ACTION_LABEL(run));
}


//
//  Hijacker_Dispatcher: C
//
// A hijacker takes over another function's identity, replacing it with its
// own implementation.  It leaves the details array intact (in case it is
// being used by some other COPY of the action), but slips its own archetype
// into the [0] slot of that array.
//
// Sometimes the hijacking function has the same underlying function
// as the victim, in which case there's no need to insert a new dispatcher.
// The hijacker just takes over the identity.  But otherwise it cannot, and
// it's not legitimate to reshape the exemplar of the victim (as something like
// an ADAPT or SPECIALIZE or a MAKE FRAME! might depend on the existing
// paramlist shape of the identity.)  Those cases need this "shim" dispatcher.
//
REB_R Hijacker_Dispatcher(REBFRM *f)
{
    REBFRM *frame_ = f;  // for RETURN macros

    // The FRM_PHASE() here is the identity that the hijacker has taken over;
    // but the actual hijacker is in the archetype.

    REBACT *phase = FRM_PHASE(f);
    REBACT *hijacker = VAL_ACTION(ACT_ARCHETYPE(phase));

    // If the hijacked function was called directly -or- by an adaptation or
    // specalization etc. which was made *after* the hijack, the frame should
    // be compatible.  Check by seeing if the keylists are derived.
    //
    REBSER *exemplar_keylist = CTX_KEYLIST(ACT_EXEMPLAR(hijacker));
    REBSER *keylist = CTX_KEYLIST(CTX(f->varlist));
    while (true) {
        if (keylist == exemplar_keylist)
            return ACT_DISPATCHER(hijacker)(f);
        if (keylist == LINK(Ancestor, keylist))  // terminates with self ref.
            break;
        keylist = LINK(Ancestor, keylist);
    }

    // Otherwise, we assume the frame was built for the function prior to
    // the hijacking...and has to be remapped.
    //
    Push_Redo_Action_Frame(OUT, f, ACT_ARCHETYPE(phase));
    delegate_subframe (FS_TOP);
}


//
//  hijack: native [
//
//  {Cause all existing references to an ACTION! to invoke another ACTION!}
//
//      return: "The hijacked action value, null if self-hijack (no-op)"
//          [<opt> action!]
//      victim "Action whose references are to be affected"
//          [action!]
//      hijacker "The action to run in its place"
//          [action!]
//  ]
//
REBNATIVE(hijack)
{
    INCLUDE_PARAMS_OF_HIJACK;

    REBACT *victim = VAL_ACTION(ARG(victim));
    REBACT *hijacker = VAL_ACTION(ARG(hijacker));

    if (victim == hijacker)
        return nullptr;  // permitting no-op hijack has some practical uses

    REBARR *victim_identity = ACT_IDENTITY(victim);
    REBARR *hijacker_identity = ACT_IDENTITY(hijacker);

    if (Action_Is_Base_Of(victim, hijacker)) {
        //
        // Should the paramlists of the hijacker and victim match, that means
        // any ADAPT or CHAIN or SPECIALIZE of the victim can work equally
        // well if we just use the hijacker's dispatcher directly.  This is a
        // reasonably common case, and especially common when putting a copy
        // of the originally hijacked function back.

        mutable_LINK_DISPATCHER(victim_identity)
            = cast(CFUNC*, LINK_DISPATCHER(hijacker_identity));
    }
    else {
        // A mismatch means there could be someone out there pointing at this
        // function who expects it to have a different frame than it does.
        // In case that someone needs to run the function with that frame,
        // a proxy "shim" is needed.
        //
        // !!! It could be possible to do things here like test to see if
        // frames were compatible in some way that could accelerate the
        // process of building a new frame.  But in general one basically
        // needs to do a new function call.
        //
        mutable_LINK_DISPATCHER(victim_identity)
            = cast(CFUNC*, &Hijacker_Dispatcher);
    }

    // The hijacker is no longer allowed to corrupt details arrays.
    // It may only move the archetype into the [0] slot of the identity.

    Copy_Cell(ACT_ARCHETYPE(victim), ACT_ARCHETYPE(hijacker));

    // !!! What should be done about MISC(victim_paramlist).meta?  Leave it
    // alone?  Add a note about the hijacking?  Also: how should binding and
    // hijacking interact?

    // We do not return a copy of the original function that can be used to
    // restore the behavior.  Because you can make such a copy yourself if
    // you intend to put the behavior back:
    //
    //     foo-saved: copy :foo
    //     hijack :foo :bar
    //     ...
    //     hijack :foo :foo-saved
    //
    // Making such a copy in this routine would be wasteful if it wasn't used.
    //
    return Init_Action(
        OUT,
        victim,
        VAL_ACTION_LABEL(ARG(victim)),
        VAL_ACTION_BINDING(ARG(hijacker))
    );
}
