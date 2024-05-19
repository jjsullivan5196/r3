#include <dirent.h>
#define FILEHANDLE_NONE -1
#define FILESIZE_UNKNOWN UINT64_MAX
#define FILEOFFSET_UNKNOWN UINT64_MAX

struct Reb_File_Port_State {
    DIR* handle; // stored during directory enumeration
    int id;      // an int, FILEHANDLE_NONE means not open

    // This is the file string in POSIX (Rebol) format, e.g. forward slashes.
    //
    // !!! Caching this as the UTF-8 extraction might seem good for efficiency,
    // but that would create a memory allocation that would have to be cleaned
    // up sometime with the port.  That's needed anyway--since a GC'd port
    // that isn't closed leaks OS handles.  But it's probably not that needed
    // since the file path extraction doesn't happen too often.
    //
    // !!! This is mutated in the case of a RENAME, which means it may be
    // changing the spec location from which it came.  That's probably not
    // ideal if the spec isn't copied/owned and might be read only (?)
    //
    Value* path;

    // !!! To the extent Ren-C can provide any value in this space at all,
    // one thing it can do is make sure it is unambiguous that all directories
    // are represented by a terminal slash.  It's an uphill battle to enforce
    // this, but perhaps a battle worth fighting.  `is_dir` should thus
    // reflect whether the last character of the path is a slash.
    //
    bool is_dir;

    // Cache of the `flags` argument passed to the open call.
    //
    // !!! Is it worth caching this, or should they be requested if needed?
    // They're not saved in the uv_fs_t req.
    //
    int flags;

    uint64_t size_cache;  // may be FILESIZE_UNKNOWN, use accessors

    uint64_t offset;
};

typedef struct Reb_File_Port_State FILEREQ;

INLINE FILEREQ *File_Of_Port(const Value* port)
{
    Value* state = CTX_VAR(VAL_CONTEXT(port), STD_PORT_STATE);
    return cast(FILEREQ*, Cell_Binary_At_Ensure_Mutable(state));
}
