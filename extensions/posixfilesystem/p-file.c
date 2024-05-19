//
//  File: %p-file.c
//  Summary: "file port interface"
//  Section: ports
//  Project: "Rebol 3 Interpreter and Run-time (Ren-C branch)"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=/////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012 REBOL Technologies
// Copyright 2012-2021 Ren-C Open Source Contributors
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
//=/////////////////////////////////////////////////////////////////////////=//
//
// FILE! ports in historical Rebol were an abstraction over traditional files.
// They did not aspire to add too much, beyond standardizing on 64-bit file
// sizes and keeping track of the idea of a "current position".
//
// The current position meant that READ or WRITE which did not provide a /SEEK
// refinement of where to seek to would use that position, and advance the
// port's index past the read or write.  But unlike with ANY-SERIES?, each
// instance of a PORT! value did not have its own index.  The position was a
// property shared among all references to a port.
//
//     rebol2>> port: skip port 10  ; you wouldn't need to write this
//     rebol2>> skip port 10        ; because this would be the same
//
// Ren-C has radically simplified R3-Alpha's implementation by standardizing on
// libuv.  There are still a tremendous number of unanswered questions about
// the semantics of FILE! ports...which ties into big questions about exactly
// "What is a PORT!":
//
//   https://forum.rebol.info/t/what-is-a-port/617
//   https://forum.rebol.info/t/semantics-of-port-s-vs-streams-vs-iterators/1689
//
// Beyond that there were many notable omissions, like FLUSH or POKE, etc.
//
//=//// NOTES //////////////////////////////////////////////////////////////=//
//
// * Some operations on files cannot be done on those files while they are
//   open, including RENAME.  The API to do a rename at the OS level just takes
//   two strings.  Yet historical Rebol still wedged this capability into the
//   port model so that RENAME is an action taken on an *unopened* port...e.g.
//   one which has merely gone through the MAKE-PORT step but not opened.
//
// * While most of the language is 1-based, the conventions for file /SEEK
//   are 0-based.  This is true also in other languages that are 1-based such
//   as Julia, Matlab, Fortran, R, and Lua:
//
//     https://discourse.julialang.org/t/why-is-seek-zero-based/55569
//

#include "reb-config.h"

#include "rebol-internals.h"

#include "tmp-paramlists.h"  // !!! for INCLUDE_PARAMS_OF_OPEN, etc.

#include "file-req.h"

#include <fcntl.h>
#include <sys/stat.h>


extern Value* Get_File_Size_Cacheable(uint64_t *size, const Value* port);
extern Value* Open_File(const Value* port, int flags);
extern Value* Close_File(const Value* port);
extern Value* Read_File(const Value* port, size_t length);
extern Value* Write_File(const Value* port, const Value* data, REBLEN length);
extern Value* Query_File_Or_Directory(const Value* port);
extern Value* Create_File(const Value* port);
extern Value* Delete_File_Or_Directory(const Value* port);
extern Value* Rename_File_Or_Directory(const Value* port, const Value* to);
extern Value* Truncate_File(const Value* port);


INLINE uint64_t File_Size_Cacheable_May_Fail(const Value* port)
{
    uint64_t size;
    Value* error = Get_File_Size_Cacheable(&size, port);
    if (error)
        fail (error);
    return size;
}


//
//  File_Actor: C
//
// Internal port handler for files.
//
Bounce File_Actor(Level* level_, Value* port, const Symbol* verb)
{
    Context* ctx = VAL_CONTEXT(port);

    // The first time the port code gets entered the state field will be NULL.
    // This code reacts to that by capturing the path out of the spec.  If the
    // operation is something like a RENAME that does not require a port to be
    // open, then this capturing of the specification is all the setup needed.
    //
    Value* state = CTX_VAR(ctx, STD_PORT_STATE);
    FILEREQ *file;
    if (Is_Binary(state)) {
        file = File_Of_Port(port);

      #if !defined(NDEBUG)
        //
        // If we think we know the size of the file, it needs to be actually
        // right...as that's where the position is put for appending and how
        // READs are clipped/etc.  Doublecheck it.
        //
        if (file->size_cache != FILESIZE_UNKNOWN) {
            assert(file->id != FILEHANDLE_NONE);

            struct stat req;
            int result = fstat(file->id, &req);
            assert(result == 0);
            assert(file->size_cache == req.st_size);
        }
      #endif
    }
    else {
        assert(Is_Nulled(state));

        Value* spec = CTX_VAR(ctx, STD_PORT_SPEC);
        if (not Is_Object(spec))
            fail (Error_Invalid_Spec_Raw(spec));

        Value* path = Obj_Value(spec, STD_PORT_SPEC_HEAD_REF);
        if (path == NULL)
            fail (Error_Invalid_Spec_Raw(spec));

        if (Is_Url(path))
            path = Obj_Value(spec, STD_PORT_SPEC_HEAD_PATH);
        else if (not Is_File(path))
            fail (Error_Invalid_Spec_Raw(path));

        // Historically the native ports would store a C structure of data
        // in a BINARY! in the port state.  This makes it easier and more
        // compact to store types that would have to be a HANDLE!.  It likely
        // was seen as having another benefit in making the internal state
        // opaque to users, so they didn't depend on it or fiddle with it.
        //
        Binary* bin = Make_Binary(sizeof(FILEREQ));
        Init_Binary(state, bin);
        Term_Binary_Len(bin, sizeof(FILEREQ));

        file = File_Of_Port(port);
        file->id = FILEHANDLE_NONE;
        file->is_dir = false;  // would be dispatching to Dir_Actor if dir
        file->size_cache = FILESIZE_UNKNOWN;
        file->offset = FILEOFFSET_UNKNOWN;

        // Generally speaking, you don't want to store Value* or Series* in
        // something like this struct-embedded-in-a-BINARY! as it will be
        // invisible to the GC.  But this pointer is into the port spec, which
        // we will assume is good for the lifetime of the port.  :-/  (Not a
        // perfect assumption as there's no protection on it.)
        //
        file->path = path;
    }

    switch (Symbol_Id(verb)) {

    //=//// REFLECT ////////////////////////////////////////////////////////=//

      case SYM_REFLECT: {
        INCLUDE_PARAMS_OF_REFLECT;

        UNUSED(ARG(value)); // implicitly comes from `port`
        Option(SymId) property = Cell_Word_Id(ARG(property));

        switch (property) {
          case SYM_OFFSET:
            return Init_Integer(OUT, file->offset);

          case SYM_LENGTH: {
            //
            // Comment said "clip at zero"
            //
            uint64_t size = File_Size_Cacheable_May_Fail(port);
            return Init_Integer(OUT, size - file->offset); }

          case SYM_HEAD:
            file->offset = 0;
            return COPY(port);

          case SYM_TAIL:
            file->offset = File_Size_Cacheable_May_Fail(port);
            return COPY(port);

          case SYM_HEAD_Q:
            return Init_Logic(OUT, file->offset == 0);

          case SYM_TAIL_Q: {
            uint64_t size = File_Size_Cacheable_May_Fail(port);
            return Init_Logic(OUT, file->offset >= size); }

          case SYM_PAST_Q: {
            uint64_t size = File_Size_Cacheable_May_Fail(port);
            return Init_Logic(OUT, file->offset > size); }

          case SYM_OPEN_Q:
            return Init_Logic(OUT, did (file->id != FILEHANDLE_NONE));

          default:
            break;
        }

        break; }

    //=//// READ ///////////////////////////////////////////////////////////=//

      case SYM_READ: {
        INCLUDE_PARAMS_OF_READ;

        UNUSED(PARAM(source));
        UNUSED(PARAM(string)); // handled in dispatcher
        UNUSED(PARAM(lines)); // handled in dispatcher

        // Handle the READ %file shortcut case, where the FILE! has been
        // converted into a PORT! but has not been opened yet.

        bool opened_temporarily;
        if (file->id != FILEHANDLE_NONE)
            opened_temporarily = false; // was already open
        else {
            Value* open_error = Open_File(port, O_RDONLY);
            if (open_error != nullptr)
                fail (Error_Cannot_Open_Raw(file->path, open_error));

            opened_temporarily = true;
        }

        Value* result;

     blockscope {
        // Seek addresses are 0-based:
        //
        // https://discourse.julialang.org/t/why-is-seek-zero-based/55569/
        //
        // !!! R3-Alpha would bound the seek to the file size; that's flaky
        // and might give people a wrong impression.  Let it error.

        if (REF(seek)) {
            int64_t seek = VAL_INT64(ARG(seek));
            if (seek <= 0)
                fail (ARG(seek));
            file->offset = seek;
        }

        // We need to know the file size in order to know either how much to
        // read (if a /PART was not supplied) or in order to bound it (the
        // /PART has traditionally meant a maximum limit, and it has not
        // errored if it gave back less).  The size might be cached in which
        // case there's no need to do a fstat (cache integrity is checked in
        // the debug build at the top of the File_Actor).
        //
        uint64_t file_size = File_Size_Cacheable_May_Fail(port);
        if (file->offset > file_size) {
            result = Init_Error(
                Alloc_Value(),
                Error_Out_Of_Range(rebValue(rebI(file->offset)))
            );
            goto cleanup_read;
        }

        // In the specific case of being at the end of file and doing a READ,
        // we return NULL.  (It is probably also desirable to follow the
        // precedent of READ-LINE and offer an end-of-file flag, so that you
        // can know if a /PART read was cut off.)
        //
        if (file_size == file->offset) {
            result = nullptr;
            goto cleanup_read;
         }

        REBLEN len = file_size - file->offset;  // default is read everything

        if (REF(part)) {
            int64_t limit = VAL_INT64(ARG(part));
            if (limit < 0) {
                result = rebValue(
                    "make error! {Negative /PART passed to READ of file}"
                );
                goto cleanup_read;
            }
            if (limit < cast(int64_t, len))
                len = limit;
        }

        result = Read_File(port, len);
     }

     cleanup_read:
        if (opened_temporarily) {
            Value* close_error = Close_File(port);
            if (result and Is_Error(result))
                fail (result);
            if (close_error)
                fail (close_error);
        }

        if (result and Is_Error(result))
            return RAISE(result);

        assert(result == nullptr or Is_Binary(result));
        return result; }

    //=//// APPEND ////////////////////////////////////////////////////////=//
    //
    // !!! R3-Alpha made APPEND to a FILE! port act as WRITE/APPEND.  This
    // raises fundamental questions regarding "is this a good idea, and
    // if so, should it be handled in a generalized way":
    //
    // https://forum.rebol.info/t/1276/14

      case SYM_APPEND: {
        INCLUDE_PARAMS_OF_APPEND;

        if (Is_Antiform(ARG(value)))
            fail (ARG(value));

        if (REF(part) or REF(dup) or REF(line))
            fail (Error_Bad_Refines_Raw());

        assert(Is_Port(ARG(series)));  // !!! poorly named
        return rebValue("write/append @", ARG(series), "@", ARG(value)); }

    //=//// WRITE //////////////////////////////////////////////////////////=//

      case SYM_WRITE: {
        INCLUDE_PARAMS_OF_WRITE;

        UNUSED(PARAM(destination));

        if (REF(seek) and REF(append))
            fail (Error_Bad_Refines_Raw());

        Value* data = ARG(data);  // binary, string, or block

        // Handle the WRITE %file shortcut case, where the FILE! is converted
        // to a PORT! but it hasn't been opened yet.

        bool opened_temporarily;
        if (file->id != FILEHANDLE_NONE) {  // already open
            //
            // !!! This checks the cached flags from opening.  But is it better
            // to just fall through to the write and let the OS error it?
            //
            if (not (
                (file->flags & O_WRONLY) or (file->flags & O_RDWR)
            )){
                fail (Error_Read_Only_Raw(file->path));
            }

            opened_temporarily = false;
        }
        else {
            int flags = 0;
            if (REF(seek)) {  // do not create
                flags |= O_WRONLY;
            }
            else if (REF(append)) {  // do not truncate
                assert(not REF(seek));  // checked above
                flags |= O_WRONLY | O_CREAT;
            }
            else
                flags |= O_WRONLY | O_CREAT | O_TRUNC;

            Value* open_error = Open_File(port, flags);
            if (open_error != nullptr)
                fail (Error_Cannot_Open_Raw(file->path, open_error));

            opened_temporarily = true;
        }

        Value* result;

      blockscope {
        uint64_t file_size = File_Size_Cacheable_May_Fail(port);

        if (REF(append)) {
            //
            // We assume WRITE/APPEND has the same semantics as WRITE/SEEK to
            // the end of the file.  This means the position before the call is
            // lost, and WRITE after a WRITE/APPEND will always write to the
            // new end of the file.
            //
            assert(not REF(seek));  // checked above
            file->offset = file_size;
        }
        else {
            // Seek addresses are 0-based:
            //
            // https://discourse.julialang.org/t/why-is-seek-zero-based/55569/
            //
            if (REF(seek)) {
                int64_t seek = VAL_INT64(ARG(seek));
                if (seek <= 0)
                    result = rebValue(
                        "make error! {Negative /PART passed to READ of file}"
                    );
                file->offset = seek;
            }

            // !!! R3-Alpha would bound the seek to the file size; that's flaky
            // and might give people a wrong impression.  Let it error.
            //
            if (file->offset > file_size) {
                result = Init_Error(
                    Alloc_Value(),
                    Error_Out_Of_Range(rebValue(rebI(file->offset)))
                );
                goto cleanup_write;
           }
        }

        REBLEN len = Part_Len_May_Modify_Index(ARG(data), ARG(part));

        if (Is_Block(data)) {  // will produce TEXT! from the data
            //
            // The conclusion drawn after much thinking about "foundational"
            // behavior is that this would not introduce spaces, e.g. it is
            // not FORM-ing but doing what appending to an empty string would.
            //
            DECLARE_MOLD (mo);
            Push_Mold(mo);

            REBLEN remain = len;  // only want as many items as in the /PART
            const Element* item = Cell_Array_Item_At(data);
            for (; remain != 0; --remain, ++item) {
                Form_Value(mo, item);
                if (REF(lines))
                    Append_Codepoint(mo->series, LF);
            }

            // !!! This makes a string all at once; could be more efficient if
            // it were written out progressively.  Also, could use the "new
            // REPEND" mechanic of GET-BLOCK! and reduce as it went.
            //
            Init_Text(data, Pop_Molded_String(mo));
            len = Cell_Series_Len_Head(data);
        }

        result = Write_File(port, data, len);
      }

      cleanup_write:

        if (opened_temporarily) {
            Value* close_error = Close_File(port);
            if (result)
                fail (result);
            if (close_error)
                fail (close_error);
        }

        if (result)
            fail (result);

        return COPY(port); }

    //=//// OPEN ///////////////////////////////////////////////////////////=//
    //
    // R3-Alpha offered a /SEEK option, which confusingly did not take a
    // parameter of where to seek in the file...but as a "hint" to say that
    // you wanted to optimize the file for seeking.  There are more such hints
    // in libuv which may be ignored or not, and probably belong under a
    // /HINT refinement if they are to be exposed:
    //
    // http://docs.libuv.org/en/v1.x/fs.html#file-open-constants
    //
    // A refinement like /RANDOM or /SEEK seem confusing (they confuse me)
    // but `/hint [sequential-access]` seems pretty clear.  TBD.

      case SYM_OPEN: {
        INCLUDE_PARAMS_OF_OPEN;

        UNUSED(PARAM(spec));

        Flags flags = 0;

        if (REF(new))
            flags |= O_CREAT | O_TRUNC;

        // The flag condition for O_RDWR is not just the OR'ing together of
        // the O_READ and O_WRITE flag, it would seem.  We tolerate the combo
        // of /READ and /WRITE even though it's the same as not specifying
        // either to make it easier for generic calling via APPLY.
        //
        if (REF(read) and REF(write)) {
            flags |= O_RDWR;
        }
        else if (REF(read)) {
            flags |= O_RDONLY;
        }
        else if (REF(write)) {
            flags |= O_WRONLY;
        }
        else
            flags |= O_RDWR;

        Value* error = Open_File(port, flags);
        if (error != nullptr)
            fail (Error_Cannot_Open_Raw(file->path, error));

        return COPY(port); }

    //=//// COPY ///////////////////////////////////////////////////////////=//
    //
    // COPY on a file port has traditionally acted as a synonym for READ.  Not
    // sure if that's a good idea or not, but this at least reduces the amount
    // of work involved by making it *actually* a synonym.

      case SYM_COPY: {
        INCLUDE_PARAMS_OF_COPY;
        UNUSED(PARAM(value));

        if (REF(deep))
            fail (Error_Bad_Refines_Raw());

        return rebValue(Canon(APPLIQUE), Canon(READ), "[",
            "source:", port,
            "part:", rebQ(ARG(part)),
        "]"); }

    //=//// CLOSE //////////////////////////////////////////////////////////=//

      case SYM_CLOSE: {
        INCLUDE_PARAMS_OF_CLOSE;
        UNUSED(PARAM(port));

        if (file->id == FILEHANDLE_NONE) {
            //
            // !!! R3-Alpha let you CLOSE an already CLOSE'd PORT!, is that
            // a good idea or should it raise an error?
        }
        else {
            Value* error = Close_File(port);
            assert(file->id == FILEHANDLE_NONE);
            if (error)
                fail (error);
        }
        return COPY(port); }

    //=//// DELETE /////////////////////////////////////////////////////////=//
    //
    // R3-Alpha did not allow you to DELETE an open port, but this considers
    // it to be the same as CLOSE and then DELETE.

      case SYM_DELETE: {
        INCLUDE_PARAMS_OF_DELETE;
        UNUSED(PARAM(port));

        if (file->id != FILEHANDLE_NONE) {
            Value* error = Close_File(port);
            if (error)
                fail (error);
        }

        Value* error = Delete_File_Or_Directory(port);
        if (error)
            fail (error);

        return COPY(port); }

    //=//// RENAME /////////////////////////////////////////////////////////=//
    //
    // R3-Alpha did not allow you to RENAME an opened port, but this will try
    // to close it, reopen it, and change the name in the spec.
    //
    // !!! To be strictly formal about it, when you close the file you lose the
    // guarantee that someone won't take a lock on it and then make it so you
    // cannot rename it and get the open access back.  Such concerns are beyond
    // the scope of this kind of codebase's concern--but just mentioning it.

      case SYM_RENAME: {
        INCLUDE_PARAMS_OF_RENAME;
        UNUSED(ARG(from));  // implicitly same as `port`

        int flags = -1;
        size_t index = -1;
        bool closed_temporarily = false;

        if (file->id != FILEHANDLE_NONE) {
            flags = file->flags;
            index = file->offset;

            Value* close_error = Close_File(port);
            if (close_error)
                fail (close_error);

            closed_temporarily = true;
        }

        Value* rename_error = Rename_File_Or_Directory(port, ARG(to));

        if (closed_temporarily) {
            Value* open_error = Open_File(port, flags);
            if (rename_error) {
                rebRelease(rename_error);
                fail (Error_No_Rename_Raw(file->path));
            }
            if (open_error)
                fail (open_error);

            file->offset = index;
        }

        if (rename_error) {
            rebRelease(rename_error);
            fail (Error_No_Rename_Raw(file->path));
        }

        Copy_Cell(file->path, ARG(to));  // !!! this mutates the spec, bad?

        return COPY(port); }

    //=//// CREATE /////////////////////////////////////////////////////////=//
    //
    // CREATE did not exist in Rebol2, and R3-Alpha seemed to use it as a
    // way of saying `open/new/read/write`.  Red does not allow CREATE to take
    // a FILE! (despite saying so in its spec).  It is removed here for now,
    // though it does seem like a nicer way of saying OPEN/NEW.
    //
    // !!! Note: reasoning of why it created a file of zero size and then
    // closed it is reverse-engineered as likely trying to parallel the CREATE
    // intent for directories.

      case SYM_CREATE: {
        fail ("CREATE on file PORT! was ill-defined, use OPEN/NEW for now"); }

    //=//// QUERY //////////////////////////////////////////////////////////=//
    //
    // The QUERY verb implemented a very limited way of asking for information
    // about files.  Ed O'Connor has proposed a much richer idea behind QUERY
    // as a SQL-inspired dialect, which could hook up to a list of properties.
    // This just gives back the size, the time, and if it's a directory or not.

      case SYM_QUERY: {
        INCLUDE_PARAMS_OF_QUERY;
        UNUSED(PARAM(target));

        Value* info = Query_File_Or_Directory(port);
        if (Is_Error(info)) {
            rebRelease(info);  // !!! R3-Alpha just returned "none"
            return nullptr;
        }

        return info ; }

    //=//// SKIP ///////////////////////////////////////////////////////////=//
    //
    // !!! While each ANY-SERIES? value in historical Rebol has its own index,
    // all instances of the same PORT! would share the same index.  This makes
    // it likely that the operation should be called something different
    // like SEEK.
    //
    // !!! Should SKIP/(SEEK) fail synchronously if you try to seek to an out
    // of bounds position, or wait to see if you skip and compensate and
    // error on the reading?

      case SYM_SKIP: {
        INCLUDE_PARAMS_OF_SKIP;

        UNUSED(PARAM(series));
        UNUSED(REF(unbounded));  // !!! Should /UNBOUNDED behave differently?

        int64_t offset = VAL_INT64(ARG(offset));
        if (offset < 0 and cast(uint64_t, -offset) > file->offset) {
            //
            // !!! Can't go negative with indices; consider using signed
            // int64_t instead of uint64_t in the files.  Problem is that
            // while SKIP for series can return NULL conservatively out of
            // range unless you use /UNBOUNDED, no similar solution exists
            // for ports since they all share the index.
            //
            return RAISE(
                Error_Out_Of_Range(rebValue(rebI(offset + file->offset)))
            );
        }

        file->offset += offset;
        return COPY(port); }

    //=//// CLEAR //////////////////////////////////////////////////////////=//
    //
    // R3-Alpha CLEAR only supported open ports.  We try working on  non-open
    // ports to just set the file to zero length.  Though the most interesting
    // case of that would be `clear %some-file.dat`, which won't work until
    // the planned removal of FILE! from ANY-STRING? (it will interpret that
    // as a request to clear the string).

      case SYM_CLEAR: {
        bool opened_temporarily = false;
        if (file->id == FILEHANDLE_NONE) {
            Value* open_error = Open_File(port, O_WRONLY);
            if (open_error)
                fail (open_error);

            opened_temporarily = true;
        }

        Value* truncate_error = Truncate_File(port);

        if (opened_temporarily) {
            Value* close_error = Close_File(port);
            if (close_error)
                fail (close_error);
        }

        if (truncate_error)
            fail (truncate_error);

        return COPY(port); }

      default:
        break;
    }

    fail (UNHANDLED);
}
