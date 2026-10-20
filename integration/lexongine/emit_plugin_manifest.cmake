if(NOT DEFINED MODULE
   OR NOT DEFINED ABI
   OR NOT DEFINED SOURCE_BUILD)
    message(FATAL_ERROR "Explicit module/build facts required")
endif()
file(SHA256 "${MODULE}" BINARY_DIGEST)
file(WRITE "${MODULE}.lexongine-build"
     "lexongine-plugin-build-v1\n${ABI}\n${SOURCE_BUILD}\n${BINARY_DIGEST}\n")
