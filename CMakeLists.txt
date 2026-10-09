# Template version: 1.0.0
# Sets the version of CMakeLists.txt file.
# Never, under any circumstances, change this line. It is used by
# the module generation script to identify the template version.

#======================== Library configuration ===============================#
# Publicly included directories to provide headers include
set( Flash_PublicIncludeDirs

)


# Privately included directories to provide headers include
set( Flash_PrivateIncludeDirs
    ${CMAKE_CURRENT_SOURCE_DIR}
)


# Source files list for current library generation
set( Flash_SourceFiles
    Flash.c
    Flash_Map.c
    Flash_Hw.c
)


# List of public header files provided by the current library
set( Flash_PublicHeaders
    Flash_Types.h
    Flash_Port.h
)


# List of dependent libraries that are publicly used by the current library
set( Flash_PublicDependentLibs
    Ral_Lib
)


# List of dependent libraries that are privately used by the current library
set( Flash_PrivateDependentLibs
    Ral_Lib
    Cache_Lib
    Nvic_Lib
    Rcc_Lib
)


#========================== Library generation ================================#
# Create library
add_library(Flash_Lib STATIC ${Flash_SourceFiles})


# Include necessary libraries to the target
target_link_libraries( Flash_Lib
    PUBLIC
        ${Flash_PublicDependentLibs}
    PRIVATE
        ${Flash_PrivateDependentLibs}
)


# Set public headers directory path in build folder
set(PUBLIC_HEADERS_DIR ${CMAKE_CURRENT_BINARY_DIR}/PublicHeaders)

# Generates the public headers directory in build folder
file(MAKE_DIRECTORY ${PUBLIC_HEADERS_DIR})

# Process all public headers
foreach(PublicHeader IN LISTS Flash_PublicHeaders)

    # Copy (or configure) only the public header there
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/${PublicHeader}
        ${PUBLIC_HEADERS_DIR}/${PublicHeader}
        COPYONLY
    )

endforeach()


# Set include directories for the created target
target_include_directories( Flash_Lib
    PUBLIC
        $<BUILD_INTERFACE:${PUBLIC_HEADERS_DIR}>
        $<INSTALL_INTERFACE:include>
        ${Flash_PublicIncludeDirs}
    PRIVATE
        ${Flash_PrivateIncludeDirs}
)


# Print status message
message(STATUS "Flash_Lib library created")

#========================= Documentation configuration ========================#
if(DOXYGEN_AVAILABLE STREQUAL "ON")
    Doxygen_AddPath("${CMAKE_CURRENT_SOURCE_DIR}/")
endif()

#========================= Unit tests configuration ===========================#
if(UNIT_TESTING_AVAILABLE STREQUAL "ON")
    UnitTesting_AddPath("${CMAKE_CURRENT_SOURCE_DIR}/Tests/UnitTests")
endif()

#===================== Integration tests configuration ========================#
if(INTEGRATION_TESTING_AVAILABLE STREQUAL "ON")
    IntegrationTesting_AddPath("${CMAKE_CURRENT_SOURCE_DIR}/Tests/IntegrationTests")
endif()
