REBOL [
    System: "REBOL [R3] Language Interpreter and Run-time Environment"
    Title: "REBOL 3 Boot Base: Other Definitions"
    Rights: {
        Copyright 2012 REBOL Technologies
        Copyright 2012-2019 Ren-C Open Source Contributors
        REBOL is a trademark of REBOL Technologies
    }
    License: {
        Licensed under the Apache License, Version 2.0
        See: http://www.apache.org/licenses/LICENSE-2.0
    }
    Description: {
        This code is evaluated just after actions, natives, sysobj, and
        other lower level definitions. This file intializes a minimal working
        environment that is used for the rest of the boot.
    }
    Note: {
        Any exported SET-WORD!s must be themselves "top level". This hampers
        procedural code here that would like to use tables to avoid repeating
        itself.  This means variadic approaches have to be used that quote
        SET-WORD!s living at the top level, inline after the function call.
    }
]

; Start with basic debugging

c-break-debug: runs :c-debug-break  ; easy to mix up

|^|: :meta
|@|: :the*

; These are faster than clear versions (e.g. `(meta void) = ^ expr`) and
; clearer than compressed forms (like '' for quote void)
;
void': meta void
null': meta null
none': meta none
nihil': meta nihil

eval: :evaluate  ; shorthands should be synonyms, too confusing otherwise

probe: func* [
    {Debug print a molded value and returns that same value.}

    return: "Same as the input value"
        [nihil? <opt> <void> any-value!]
    ^value' [<opt> <void> pack? raised? any-value! barrier?]
][
    ; Remember this is early in the boot, so many things not defined.

    write-stdout case [
        value' = void' ["; void"]
        quasi? value' [unspaced [mold value' space space "; isotope"]]
    ] else [
        mold unmeta value'
    ]
    write-stdout newline

    return unmeta value'
]

??: runs :probe  ; shorthand for debug sessions, not intended to be committed

; Pre-decaying specializations for DID, DIDN'T, THEN, ELSE, ALSO
;
; https://forum.rebol.info/t/why-then-and-else-are-mutually-exclusive/1080/9
;
did*: runs :did/decay
didn't*: runs :didn't/decay
*then: enfix :then/decay
*also: enfix :also/decay
*else: enfix :else/decay

; Give special operations their special properties
;
; !!! There may be a function spec property for these, but it's not currently
; known what would be best for them.  They aren't parameter conventions, they
; apply to the whole action.
;
tweak :then 'defer on
tweak :also 'defer on
tweak :else 'defer on
tweak :except 'defer on
tweak :*then 'defer on
tweak :*also 'defer on
tweak :*else 'defer on


; ARITHMETIC OPERATORS
;
; Note that `/` is rather trickily not a PATH!, but a decayed form as a WORD!

+: enfix :add
-: enfix :subtract
*: enfix :multiply
/: enfix :divide


; SET OPERATORS

not+: runs :bitwise-not
and+: enfix :bitwise-and
or+: enfix :bitwise-or
xor+: enfix :bitwise-xor
and-not+: enfix :bitwise-and-not


; COMPARISON OPERATORS
;
; !!! See discussion about the future of comparison operators:
; https://forum.rebol.info/t/349

=: enfix :equal?
<>: enfix :not-equal?
<: enfix :lesser?
>: enfix :greater?

; "Official" forms of the comparison operators.  This is what we would use
; if starting from scratch, and didn't have to deal with expectations people
; have coming from other languages: https://forum.rebol.info/t/349/
;
>=: enfix :greater-or-equal?
=<: enfix :equal-or-lesser?

; Compatibility Compromise: sacrifice what looks like left and right arrows
; for usage as comparison, even though the perfectly good `=<` winds up
; being unused as a result.  Compromise `=>` just to reinforce what is lost
; by not retraining: https://forum.rebol.info/t/349/11
;
equal-or-greater?: runs :greater-or-equal?
lesser-or-equal?: runs :equal-or-lesser?
=>: enfix :equal-or-greater?
<=: enfix :lesser-or-equal?

!=: enfix :not-equal?  ; http://www.rebol.net/r3blogs/0017.html
==: enfix :strict-equal?  ; !!! https://forum.rebol.info/t/349
!==: enfix :strict-not-equal?  ; !!! bad pairing, most would think !=

=?: enfix :same?


; Common "Invisibles"

comment: func* [
    {Ignores the argument value, but does no evaluation (see also ELIDE)}

    return: "Evaluator will skip over the result (not seen)"
        [nihil?]
    :discarded "Literal value to be ignored."  ; `comment print "x"` disallowed
        [block! any-string! binary! any-scalar!]
][
    return nihil
]

elide: func* [
    {Argument is evaluative, but discarded (see also COMMENT)}

    return: "The evaluator will skip over the result (not seen)"
        [nihil?]
    ^discarded "Evaluated value to be ignored"
        [<opt> <void> pack? any-value!]  ; pack? so (elide elide "x") works
][
    return nihil
]

elide-if-void: func* [
    {Argument is evaluative, but discarded if void}

    return: [nihil? <opt> <void> any-value!]
    ^value' "Evaluated value to be ignored"
        [<opt> <void> pack? any-value!]  ; pack? is passed through
][
    if value' = void' [return nihil]
    return unmeta value'
]

; COMMA! is the new expression barrier.  But `||` is included as a way to
; make comma isotopes to show how to create custom barrier-like constructs.
;
|\|\||: func* [] [return ~,~]

|\|\|\||: func* [  ; e.g. |||
    {Inertly consumes all subsequent data, evaluating to previous result.}

    return: <nihil>
    :omit [any-value! <variadic>]
][
    until [null? try take omit]
]


; EACH will ultimately be a generator, but for now it acts as QUOTE so it can
; be used with `map x each [a b c] [...]` and give you x as a, then b, then c.
;
each: runs :quote


; Function derivations have core implementations (SPECIALIZE*, ADAPT*, etc.)
; that don't create META information for the HELP.  Those can be used in
; performance-sensitive code.
;
; These higher-level variations without the * (SPECIALIZE, ADAPT, etc.) do the
; inheritance for you.  This makes them a little slower, and the generated
; functions will be bigger due to having their own objects describing the
; HELP information.  That's not such a big deal for functions that are made
; only one time, but something like a KEEP inside a COLLECT might be better
; off being defined with ENCLOSE* instead of ENCLOSE and foregoing HELP.
;
; Once HELP has been made for a derived function, it can be customized via
; REDESCRIBE.
;
; https://forum.rebol.info/t/1222
;
; Note: ENCLOSE is the first wrapped version here; so that the other wrappers
; can use it, thus inheriting HELP from their core (*-having) implementations.
;
; (A usermode INHERIT-ADJUNCT existed, but it was slow.  It was made native.)
;
; https://forum.rebol.info/t/1619
;

enclose: enclose* :enclose* lambda [f] [  ; uses low-level ENCLOSE* to make
    let inner: f.inner
    inherit-adjunct (do f) inner
]
inherit-adjunct enclose :enclose*  ; needed since we used ENCLOSE*

specialize: enclose :specialize* lambda [f] [  ; now we have high-level ENCLOSE
    let original: f.original
    inherit-adjunct (do f) original
]

adapt: enclose :adapt* lambda [f] [
    let original: f.original
    inherit-adjunct (do f) original
]

chain: enclose :chain* lambda [f] [
    let pipeline1: pick (f.pipeline: reduce/predicate f.pipeline :unrun) 1
    inherit-adjunct (do f) pipeline1
]

augment: enclose :augment* lambda [f] [
    let original: f.original
    let spec: f.spec
    inherit-adjunct/augment (do f) original spec
]

reframer: enclose :reframer* lambda [f] [
    let shim: f.shim
    inherit-adjunct (do f) shim
]

reorder: enclose :reorder* lambda [f] [
    let original: f.original
    inherit-adjunct (do f) original
]


; REQUOTE is helpful when functions do not accept QUOTED! values.
;
requote: reframer lambda [
    {Remove Quoting Levels From First Argument and Re-Apply to Result}
    f [frame!]
    <local> p num-quotes result
][
    if not p: first parameters of f [
        fail ["REQUOTE must have an argument to process"]
    ]

    num-quotes: quotes of f.(p)

    f.(p): noquote f.(p)

    light (do f then result -> [  ; !!! proper light-null handling here?
        quote/depth get/any 'result num-quotes
    ] else [null])
]


; If <end> is used, e.g. `x: -> [print "hi"]` then this will act like DOES.
; (It's still up in the air whether DOES has different semantics or not.)
;
->: enfix lambda [
    'words "Names of arguments (will not be type checked)"
        [<skip> word! lit-word! meta-word! refinement! block! group!]
    body "Code to execute"
        [block!]
][
    if group? words [words: eval words]
    lambda words body
]


; !!! NEXT and BACK seem somewhat "noun-like" and desirable to use as variable
; names, but are very entrenched in Rebol history.  Also, since they are
; specializations they don't fit easily into the NEXT OF SERIES model--this
; is a problem which hasn't been addressed.
;
next: specialize :skip [offset: 1]
back: specialize :skip [offset: -1]

; Function synonyms

min: runs :minimum
max: runs :maximum
abs: runs :absolute

unspaced: specialize :delimit [delimiter: null]
spaced: specialize :delimit [delimiter: space]
newlined: specialize :delimit [delimiter: newline, tail: #]

an: lambda [
    {Prepends the correct "a" or "an" to a string, based on leading character}
    value <local> s
][
    if null? value [fail 'value]
    head of insert (s: form value) either (find "aeiou" s.1) ["an "] ["a "]
]


; !!! REDESCRIBE not defined yet
;
; head?
; {Returns TRUE if a series is at its beginning.}
; series [any-series! port!]
;
; tail?
; {Returns TRUE if series is at or past its end; or empty for other types.}
; series [any-series! object! port! bitset! map! blank! varargs!]
;
; past?
; {Returns TRUE if series is past its end.}
; series [any-series! port!]
;
; open?
; {Returns TRUE if port is open.}
; port [port!]

head?: specialize :reflect [property: 'head?]
tail?: specialize :reflect [property: 'tail?]
past?: specialize :reflect [property: 'past?]
open?: specialize :reflect [property: 'open?]


empty?: func* [
    {TRUE if blank, or if series is empty or at or beyond its tail}
    return: [logic?]
    series [blank! any-series! object! port! bitset! map!]
][
    return did any [blank? series, tail? series]
]

empty-or-null?: func* [
    {TRUE if null, blank, or if series is empty or at or beyond its tail}
    return: [logic?]
    series [<opt> blank! any-series! object! port! bitset! map!]
][
    return did any [null? series, blank? series, tail? series]
]


run func* [
    {Make fast type testing functions (variadic to quote "top-level" words)}
    return: <none>
    'set-words [<variadic> set-word! tag!]
    <local>
        set-word type-name tester meta
][
    while [<end> != set-word: take set-words] [
        type-name: copy as text! set-word
        change back tail of type-name "!"  ; change ? at tail to !
        tester: unrun typechecker (get bind (as word! type-name) set-word)
        set set-word runs tester

        set-adjunct tester make system.standard.action-adjunct [
            description: spaced [{Returns TRUE if the value is} an type-name]
            return-type: [logic!]
        ]
    ]
]
    blank?:
    comma?:
    integer?:
    decimal?:
    percent?:
    money?:
    pair?:
    time?:
    date?:
    word?:
    set-word?:
    get-word?:
    meta-word?:
    the-word?:
    type-word?:
    issue?:
    binary?:
    text?:
    file?:
    email?:
    url?:
    tag?:
    bitset?:
    path?:
    set-path?:
    get-path?:
    meta-path?:
    the-path?:
    type-path?:
    tuple?:
    set-tuple?:
    get-tuple?:
    meta-tuple?:
    the-tuple?:
    type-tuple?:
    block?:
    set-block?:
    get-block?:
    meta-block?:
    the-block?:
    type-block?:
    group?:
    get-group?:
    set-group?:
    meta-group?:
    the-group?:
    type-group?:
    map?:
    varargs?:
    object?:
    frame?:
    module?:
    error?:
    port?:
    handle?:

    <end>


; bridge compatibility, as LIT-WORD! and LIT-PATH! are no longer fundamental
; datatypes... but type constraints (LIT-WORD? and LIT-PATH?)

to-lit-word: func* [return: [quoted!] value [any-value!]] [
    return quote to word! noquote value
]

to-lit-path: func* [return: [quoted!] value [any-value!]] [
    return quote to path! noquote value
]

print: func* [
    {Output SPACED text with newline (evaluating elements if BLOCK!)}

    return: "See NIHIL docs for caution on casually making vaporizing routines"
        [nihil?]
    line "Line of text or block, [] has NO output, CHAR! newline allowed"
        [<maybe> char! text! block! quoted!]
][
    if char? line [
        if line <> newline [
            fail "PRINT only allows CHAR! of newline (see WRITE-STDOUT)"
        ]
        write-stdout line
        return nihil
    ]

    if quoted? line [  ; Speculative feature: quote mark as a mold request
        line: mold unquote line
    ]

    write-stdout (maybe spaced line) then [
        write-stdout newline
    ]

    return nihil
]

echo: func* [
    {Freeform output of text, with @WORD, @TU.P.LE, and @(GR O UP) as escapes}

    return: <nihil>
    'args "If a BLOCK!, then just that block's contents--else to end of line"
        [any-value! <variadic>]
    <local> line
][
    line: if block? first args [take args] else [
        collect [
            cycle [
                case [
                    tail? args [stop]
                    new-line? args [stop]
                    comma? first args [stop]
                ]
                keep take args
            ]
        ]
    ]
    write-stdout form map-each item line [
        switch/type item [
            the-word! [get item]
            the-tuple! [get item]
            the-group! [do as block! item]
        ] else [
            item
        ]
    ]
    write-stdout newline
]


internal!: &[
    handle!
]

immediate!: &[  ; Does not include internal datatypes
    blank! logic! any-scalar! date! any-word!
]

ok?: func* [
    "Returns TRUE on all values that are not ERROR!"
    return: [logic?]
    value [<opt> any-value!]
][
    return not error? :value
]

; Convenient alternatives for readability
;
neither?: runs :nand?
both?: runs :and?
