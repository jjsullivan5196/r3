//
//  File: %file-posix.c
//  Summary: "Interface to the POSIX filesystem API."
//  Project: "Rebol 3 Interpreter and Run-time (Ren-C branch)"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
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
//=////////////////////////////////////////////////////////////////////////=//

#include "reb-config.h"

#include <time.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#include "rebol-internals.h"

#include "file-req.h"

#ifndef PATH_MAX
    #define PATH_MAX 4096  // generally lacking in Posix
#endif

//
//  rebError_PosixErrno: C
//
//  Get the last error reported by the OS.
//
Value* rebError_POSIXErrno() {
    return rebValue("make error!", rebT(strerror(errno)));
}

//
//  Get_File_Size_Cacheable: C
//
// If the file size hasn't been queried (because it wasn't needed) then do
// an fstat() to get the information.
//
Value* Get_File_Size_Cacheable(uint64_t *size, const Value* port)
{
    FILEREQ *file = File_Of_Port(port);

    if (file->size_cache != FILESIZE_UNKNOWN) {
        *size = file->size_cache;
        return nullptr;  // assume accurate (checked each entry to File_Actor)
    }

    struct stat req;
    int result = fstat(file->id, &req);
    if (result != 0) {
        *size = FILESIZE_UNKNOWN;
        return rebError_POSIXErrno();
    }

    *size = req.st_size;
    return nullptr;
}


//
//  Try_Read_Directory_Entry: C
//
// This function will read a file directory, one file entry at a time, then
// close when no more files are found.  The value returned is an API handle
// of a FILE!, nullptr if there's no more left, or an ERROR!.
//
// !!! R3-Alpha comment said: "The dir->path can contain wildcards * and ?.
// The processing of these can be done in the OS (if supported) or by a
// separate filter operation during the read."  How does libuv handle this?
//
Value* Try_Read_Directory_Entry(FILEREQ *dir)
{
    assert(dir->is_dir);

    // If no dir enumeration handle (e.g. this is the first Try_Read_Directory()
    // call in a batch that expects to keep calling until done) open the dir
    //
    if (dir->handle == nullptr) {
        char *dir_utf8 = rebSpell("file-to-local", dir->path);
        dir->handle = opendir(dir_utf8);

        rebFree(dir_utf8);

        if (dir->handle == nullptr)
            return rebError_POSIXErrno();
    }

    // Get dir entry (skip over the . and .. dir cases):
    //
    struct dirent* entry;
    char* ename;
    do {
        errno = 0;
        entry = readdir(dir->handle);

	if (entry == nullptr) {
	    closedir(dir->handle);
	    dir->handle = nullptr;

	    if (errno != 0) {
		return rebError_POSIXErrno();
	    }
	    else {
		return nullptr;
	    }
	}

	ename = entry->d_name;
    } while (
        ename[0] == '.' and (
            ename[1] == '\0'
            or (ename[1] == '.' and ename[2] == '\0')
        )
    );

    // !!! R3-Alpha had a limited model and only recognized directory and file.
    // readdir can enumerate symbolic links in addition to files and directories.
    // Review the exposure of that!
    //
    struct stat estat;

    char* path_utf8 = rebSpell("join (file-to-local", dir->path, ")", rebT(ename));

    stat(path_utf8, &estat);

    rebFree(path_utf8);

    bool is_dir = S_ISDIR(estat.st_mode);

    Value* path = rebValue(
        "applique :local-to-file [",
	    "path: join", rebT(ename), is_dir ? "{/}" : "{}",
	    "dir: all [", rebL(is_dir), "#]",
        "]"
    );

    return path;
}


//
//  Open_File: C
//
// Open the specified file with the given flags.
//
Value* Open_File(const Value* port, int flags)
{
    FILEREQ *file = File_Of_Port(port);

    if (file->id != FILEHANDLE_NONE)
        return rebValue("make error! {File is already open}");

    // "Posix file names should be compatible with REBOL file paths"

    assert(file->id == FILEHANDLE_NONE);
    assert(file->size_cache == FILESIZE_UNKNOWN);
    assert(file->offset == FILEOFFSET_UNKNOWN);

    // "mode must be specified when O_CREAT is in the flags, and is ignored
    // otherwise."  Although the parameter is named singularly, it is the
    // result of a bitmask of flags.
    //
    // !!! libuv does not seem to provide these despite providing UV_FS_O_XXX
    // constants.  Would anything bad happen if we left it at 0?
    //
    int mode = 0;
    if (flags & O_CREAT) {
        if (flags & O_RDONLY)
            mode = S_IREAD;
        else {
            mode = S_IREAD | S_IWRITE | S_IRGRP | S_IWGRP | S_IROTH;
        }
    }

    char *path_utf8 = rebSpell("file-to-local/full", file->path);

    int h;
    h = open(path_utf8, flags, mode);

    rebFree(path_utf8);

    if (h < 0)
        return rebError_POSIXErrno();

    // Note: this code used to do an lseek() to "confirm that a seek-mode file
    // is actually seekable".  libuv does not offer lseek, apparently because
    // it is contentious with asynchronous I/O.
    //
    // Note2: this code also used to fetch the file size with fstat.  It's not
    // clear why it would need to proactively do that.
    //
    file->id = h;
    file->offset = 0;
    file->flags = flags;
    assert(file->size_cache == FILESIZE_UNKNOWN);

    return nullptr;
}


//
//  Close_File: C
//
// Closes a previously opened file.
//
Value* Close_File(const Value* port)
{
    FILEREQ *file = File_Of_Port(port);

    assert(file->id != FILEHANDLE_NONE);

    int result = close(file->id);

    file->id = FILEHANDLE_NONE;
    file->offset = FILEOFFSET_UNKNOWN;
    file->size_cache = FILESIZE_UNKNOWN;

    if (result < 0)
        return rebError_POSIXErrno();

    return nullptr;
}


//
//  Read_File: C
//
Value* Read_File(const Value* port, size_t length)
{
    FILEREQ *file = File_Of_Port(port);

    assert(not file->is_dir);  // should call Read_Directory!
    assert(file->id != FILEHANDLE_NONE);

    // Make buffer for read result that can be "repossessed" as a BINARY!
    //
    char *buffer = rebAllocN(char, length);
    ssize_t num_bytes_read = read(file->id, buffer, length);

    if (num_bytes_read < 0) {
        rebFree(buffer);
        return rebError_POSIXErrno();
    }

    file->offset += num_bytes_read;

    // !!! The read is probably frequently shorter than the buffer size that
    // was allocated, so the space should be reclaimed...though that should
    // probably be something the GC does when it notices oversized series
    // just as a general cleanup task.
    //
    return rebRepossess(buffer, num_bytes_read);
}


//
//  Write_File: C
//
Value* Write_File(const Value* port, const Value* value, REBLEN limit)
{
    FILEREQ *file = File_Of_Port(port);

    assert(file->id != FILEHANDLE_NONE);

    if (limit == 0) {
        //
        // !!! While it may seem like writing a length of 0 could be shortcut
        // here, it is actually the case that 0 byte writes can have meaning
        // to some receivers of pipes.  Use cases should be studied before
        // doing a shortcut here.
    }

    const Byte* data;
    size_t size;

    if (Is_Text(value) or Is_Issue(value)) {
        Utf8(const*) utf8 = Cell_Utf8_Len_Size_At_Limit(
            nullptr,
            &size,
            value,
            limit
        );

        // !!! In the quest to purify the universe, we've been checking to
        // make sure that strings containing CR are not written out if you
        // are writing "text".  You have to send BINARY! (which can be done
        // cheaply with an alias, AS TEXT!, uses the same memory)
        //
        const Byte* tail = c_cast(Byte*, utf8) + size;
        const Byte* pos = utf8;
        for (; pos != tail; ++pos)
            if (*pos == CR)
                fail (Error_Illegal_Cr(pos, utf8));

        data = utf8;
    }
    else {
        if (not Is_Binary(value))
            return rebValue("make error! {ISSUE!, TEXT!, BINARY! for WRITE}");

        data = Cell_Binary_At(value);
        size = limit;
    }

    ssize_t num_bytes_written = write(file->id, (void*)m_cast(void*, cs_cast(data)), size);

    if (num_bytes_written < 0) {
        file->size_cache = FILESIZE_UNKNOWN;  // don't know what fail did
        return rebError_POSIXErrno();
    }

    assert(num_bytes_written == cast(ssize_t, size));

    file->offset += num_bytes_written;

    // !!! The concept of R3-Alpha was that it would keep the file size up to
    // date...theoretically.  But it actually didn't do that here.  Adding it,
    // but also adding a check in File_Actor() to make sure the cache is right.
    //
    if (file->size_cache != FILESIZE_UNKNOWN) {
        if (file->offset + num_bytes_written > file->size_cache) {
            file->size_cache += (
                num_bytes_written - (file->size_cache - file->offset)
            );
        }
   }

    return nullptr;
}


//
//  Truncate_File: C
//
Value* Truncate_File(const Value* port)
{
    FILEREQ *file = File_Of_Port(port);
    assert(file->id != FILEHANDLE_NONE);

    int result = ftruncate(file->id, file->offset);
    if (result != 0)
        return rebError_POSIXErrno();

    return nullptr;
}


//
//  Create_Directory: C
//
Value* Create_Directory(const Value* port)
{
    FILEREQ *dir = File_Of_Port(port);
    assert(dir->is_dir);

    // !!! We use /NO-TAIL-SLASH here because there was some historical issue
    // about leaving the tail slash on calling mkdir() on some implementation.
    //
    char *path_utf8 = rebSpell("file-to-local/full/no-tail-slash", dir->path);

    int result = mkdir(path_utf8, 0777);

    rebFree(path_utf8);

    if (result != 0)
	return rebError_POSIXErrno();

    return nullptr;
}


//
//  Delete_File_Or_Directory: C
//
// Note: Directories must be empty to succeed
//
Value* Delete_File_Or_Directory(const Value* port)
{
    FILEREQ *file = File_Of_Port(port);

    // !!! There is a /NO-TAIL-SLASH refinement, but the tail slash was left on
    // for directory removal, because it seemed to be supported.  Review if
    // there is any reason to remove it.
    //
    char *path_utf8 = rebSpell("file-to-local/full", file->path);

    int result;
    if (file->is_dir)
        result = rmdir(path_utf8);
    else
        result = unlink(path_utf8);

    rebFree(path_utf8);

    if (result != 0)
	return rebError_POSIXErrno();

    return nullptr;
}


//
//  Rename_File_Or_Directory: C
//
Value* Rename_File_Or_Directory(const Value* port, const Value* to)
{
    FILEREQ *file = File_Of_Port(port);

    char *from_utf8 = rebSpell("file-to-local/full/no-tail-slash", file->path);
    char *to_utf8 = rebSpell("file-to-local/full/no-tail-slash", to);

    int result = rename(from_utf8, to_utf8);

    rebFree(to_utf8);
    rebFree(from_utf8);

    if (result != 0)
	return rebError_POSIXErrno();

    return nullptr;
}


#ifndef timeval
    #include <sys/time.h>  // for older systems
#endif

//
//  Get_Timezone: C
//
// Get the time zone in minutes from GMT.
// NOT consistently supported in Posix OSes!
// We have to use a few different methods.
//
// !!! "local_tm->tm_gmtoff / 60 would make the most sense,
// but is no longer used" (said a comment)
//
// !!! This code is currently repeated in the time extension, until a better
// way of sharing it is accomplished.
//
static int Get_Timezone(struct tm *utc_tm_unused)
{
    time_t now_secs;
    time(&now_secs); // UNIX seconds (since "epoch")
    struct tm local_tm = *localtime(&now_secs);

#if !defined(HAS_SMART_TIMEZONE)
    //
    // !!! The R3-Alpha host code would always give back times in UTC plus
    // timezone.  Then, functions like NOW would have ways of adjusting for
    // the timezone (unless you asked to do something like NOW/UTC), but
    // without taking daylight savings time into account.
    //
    // We don't want to return a fake UTC time to the caller for the sake of
    // keeping the time zone constant.  So this should return e.g. GMT-7
    // during pacific daylight time, and GMT-8 during pacific standard time.
    // Get that effect by erasing the is_dst flag out of the local time.
    //
    local_tm.tm_isdst = 0;
#endif

    // mktime() function inverts localtime()... there is no equivalent for
    // gmtime().  However, we feed it gmtime() as if it were the localtime.
    // Then the time zone can be calculated by diffing it from a mktime()
    // inversion of a suitable local time.
    //
    // !!! For some reason, R3-Alpha expected the caller to pass in a utc
    // tm structure pointer but then didn't use it, choosing to make
    // another call to gmtime().  Review.
    //
    UNUSED(utc_tm_unused);
    time_t now_secs_gm = mktime(gmtime(&now_secs));

    double diff = difftime(mktime(&local_tm), now_secs_gm);
    return cast(int, diff / 60);
}

//
//  File_Time_To_Rebol: C
//
// Convert file.time to REBOL date/time format.
// Time zone is UTC.
//
Value* File_Time_To_Rebol(time_t stime)
{
    // gmtime() is badly named.  It's utc time.  Note we have to be careful
    // as it returns a system static buffer, so we have to copy the result
    // via dereference to avoid calls to localtime() in Get_Timezone()
    // from corrupting the buffer before it gets used.
    //
    // !!! Consider usage of the thread-safe variants, though they are not
    // available on all older systems.
    //
    struct tm utc_tm = *gmtime(&stime);

    int zone = Get_Timezone(&utc_tm);

    return rebValue("ensure date! (make-date-ymdsnz",
        rebI(utc_tm.tm_year + 1900),  // year
        rebI(utc_tm.tm_mon + 1),  // month
        rebI(utc_tm.tm_mday),  // day
        rebI(
            utc_tm.tm_hour * 3600
            + utc_tm.tm_min * 60
            + utc_tm.tm_sec
        ),  // secs
        rebI(0),  // nanoseconds (file times don't have this)
        rebI(zone),  // zone
    ")");
}

//
//  Query_File_Or_Directory: C
//
// Obtain information about a file.  Produces a STD_FILE_INFO object.
//
Value* Query_File_Or_Directory(const Value* port)
{
    FILEREQ *file = File_Of_Port(port);

    // The original implementation here used /no-trailing-slash for the
    // FILE-TO-LOCAL, which meant that %/ would turn into an empty string.
    // It would appear that for directories, trailing slashes are acceptable
    // in `stat`...though for symlinks different answers are given based
    // on the presence of the slash:
    //
    // https://superuser.com/questions/240743/
    //
    char *path_utf8 = rebSpell("file-to-local/full", file->path);

    struct stat req;
    int result = stat(path_utf8, &req);

    rebFree(path_utf8);

    if (result != 0)
      return rebError_POSIXErrno();

    bool is_dir = S_ISDIR(req.st_mode);
    if (is_dir != file->is_dir)
        return rebValue("make error! {Directory/File flag mismatch}");

    // !!! R3-Alpha would do this "to be consistent on all systems".  But it
    // seems better to just make the size null, unless there is some info
    // to be gleaned from a directory's size?
    //
    //     if (is_dir)
    //         req.statbuf.st_size = 0;

    // Note: time is in local format and must be converted
    //
    Value* timestamp = File_Time_To_Rebol(req.st_mtime);

    return rebValue(
        "make ensure object! (", port , ").scheme.info [",
            "name:", file->path,
            "size:", is_dir ? rebQ(nullptr) : rebI(req.st_size),
            "type:", is_dir ? "'dir" : "'file",
            "date:", rebR(timestamp),
        "]"
    );
}


//
//  Get_Current_Dir_Value: C
//
// Result is a FILE! API Handle, must be freed with rebRelease()
//
Value* Get_Current_Dir_Value(void)
{
    char *path_utf8 = rebAllocN(char, PATH_MAX);

    size_t size = PATH_MAX - 1;
    getcwd(path_utf8, size);
    size = strlen(path_utf8) + 1;
    path_utf8 = s_cast(rebReallocBytes(path_utf8, size));  // includes \0

    // "On Unix the path no longer ends in a slash"...the /DIR option should
    // make it end in a slash for the result.

    Value* result = rebValue("local-to-file/dir", rebT(path_utf8));

    rebFree(path_utf8);
    return result;
}

//
//  Set_Current_Dir_Value: C
//
// Set the current directory to local path. Return FALSE on failure.
//
bool Set_Current_Dir_Value(const Value* path)
{
    char *path_utf8 = rebSpell("file-to-local/full", path);

    int result = chdir(path_utf8);

    rebFree(path_utf8);

    return result == 0;  // !!! return ERROR! value instead?
}

#ifdef __COSMOPOLITAN__
  #include <libc/cosmo.h>
#endif

//
//  Get_Current_Exec: C
//
Value* Get_Current_Exec(void)
{
  #ifdef __COSMOPOLITAN__
    return rebValue("local-to-file", rebT(GetProgramExecutableName()));
  #else
    return nullptr;
  #endif
}
