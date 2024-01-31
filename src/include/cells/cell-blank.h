//
//  File: %cell-blank.h
//  Summary: "BLANK! inert placeholder type"
//  Project: "Rebol 3 Interpreter and Run-time (Ren-C branch)"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012-2023 Ren-C Open Source Contributors
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
// BLANK! cells are inert in the evaluator, and represented by an underscore.
// They are used as agnostic placeholders.
//
//    >> append [a b c] _
//    == [a b c _]
//
// BLANK! takes on the placeholder responsibilities of Rebol2's #[none]
// value, while the "soft failure" aspects are covered by NULL (which unlike
// blanks, can't be stored in blocks).  Consequently blanks are not "falsey"
// which means all "reified" values that can be stored in blocks are
// conditionally true.
//
//     >> if fourth [a b c _] [print "Blanks are truthy"]
//     Blanks are truthy
//
// Aiding in blank's usefulness as a placeholder, SPREAD of BLANK! gives
// back the same behavior as if you were to SPREAD an empty block:
//
//    >> append [d e] spread fourth [a b c []]
//    == [d e]
//
//    >> append [d e] spread fourth [a b c _]
//    == [d e]
//
//=//// NOTES /////////////////////////////////////////////////////////////=//
//
// * A speculative feature for blanks is to consider them as spaces when
//   dealing with string operations:
//
//       >> append "ab" _
//       == "ab "
//
//       >> parse "a b" ["a" _ "b"]
//       == "b"
//
//   There are benefits and drawbacks to being casual about tihs conversion,
//   so at time of writing, it's not certain if this will be kept.
//
// * Some alternative placeholder values are quoted voids (represented by a
//   lone apostrophe) and quasi voids (represented by a lone tilde).  These
//   have different behavior, e.g. SPREAD of a ~ is an error
//

INLINE Element* Init_Blank_Untracked(Cell* out, Byte quote_byte) {
    FRESHEN_CELL(out);
    out->header.bits |= (
        NODE_FLAG_NODE | NODE_FLAG_CELL
            | FLAG_HEART_BYTE(REB_BLANK) | FLAG_QUOTE_BYTE(quote_byte)
    );

  #ifdef ZERO_UNUSED_CELL_FIELDS
    EXTRA(Any, out).corrupt = CORRUPTZERO;  // not Cell_Extra_Needs_Mark()
    PAYLOAD(Any, out).first.corrupt = CORRUPTZERO;
    PAYLOAD(Any, out).second.corrupt = CORRUPTZERO;
  #endif

    return cast(Element*, out);
}

#define Init_Blank(out) \
    TRACK(Init_Blank_Untracked((out), NOQUOTE_1))

#define Init_Quasi_Blank(out) \
    TRACK(Init_Blank_Untracked((out), QUASIFORM_2))

INLINE bool Is_Quasi_Blank(const Cell* v)
  { return Is_Quasiform(v) and HEART_BYTE(v) == REB_BLANK; }
