
# Compiler src
LOCAL_SRC_FILES := \
    BuiltInFunctionEmulator.cpp \
    Compiler.cpp \
    DetectRecursion.cpp \
    ForLoopUnroll.cpp \
    InfoSink.cpp \
    Initialize.cpp \
    InitializeDll.cpp \
    IntermTraverse.cpp \
    Intermediate.cpp \
    MapLongVariableNames.cpp \
    ParseHelper.cpp \
    PoolAlloc.cpp \
    QualifierAlive.cpp \
    RemoveTree.cpp \
    ShaderLang.cpp \
    SymbolTable.cpp \
    ValidateLimitations.cpp \
    VariableInfo.cpp \
    debug.cpp \
    intermOut.cpp \
    ossource_posix.cpp \
    parseConst.cpp \
    util.cpp

# Code generator
LOCAL_SRC_FILES := $(LOCAL_SRC_FILES) \
    CodeGenGLSL.cpp \
    OutputESSL.cpp \
    OutputGLSL.cpp \
    OutputGLSLBase.cpp \
    TranslatorESSL.cpp \
    TranslatorGLSL.cpp \
    VersionGLSL.cpp

# Generated files
LOCAL_SRC_FILES := $(LOCAL_SRC_FILES) \
    glslang_lex.cpp \
    glslang_tab.cpp

# Preprocessor
LOCAL_SRC_FILES := $(LOCAL_SRC_FILES) \
    preprocessor/atom.c \
    preprocessor/cpp.c \
    preprocessor/cppstruct.c \
    preprocessor/memory.c \
    preprocessor/scanner.c \
    preprocessor/symbols.c \
    preprocessor/tokens.c
