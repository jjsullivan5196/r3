REBOL []

target: 'execution
standard: 'c
optimize: 2
debug: 'asserts

cc: get-env "CC"

toolset: compose [
  gcc   (cc)
  ld    (cc)
  strip _
]

extensions: make map! [
    Clipboard -
    Console +
    Crypt +
    Debugger +
    DNS -
    Filesystem -
    PosixFilesystem +
    JavaScript -
    Locale +
    Network -
    ODBC -
    Process +
    TCC -
    Time +
    UUID -
    UTF +
    View -
]
