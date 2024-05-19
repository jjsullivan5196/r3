REBOL []

name: 'PosixFilesystem
source: %posixfilesystem/mod-posixfilesystem.c
includes: [
    %prep/extensions/posixfilesystem
]

depends: compose [
    %posixfilesystem/p-file.c
    %posixfilesystem/p-dir.c
    %posixfilesystem/file-posix.c
]