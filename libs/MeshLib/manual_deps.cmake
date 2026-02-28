# --- Manual Dependency Integration ---
if(NOT MESHLIB_USE_VCPKG)
    message(STATUS "Integrating manual dependencies from ${MESHLIB_THIRDPARTY_DIR}")
    
    # Boost
    if(NOT TARGET Boost::headers)
        add_library(Boost::headers INTERFACE IMPORTED)
        set_target_properties(Boost::headers PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${MESHLIB_THIRDPARTY_DIR}/boost")
    endif()
    
    # Eigen3
    if(NOT TARGET Eigen3::Eigen)
        add_library(Eigen3::Eigen INTERFACE IMPORTED)
        set_target_properties(Eigen3::Eigen PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${MESHLIB_THIRDPARTY_DIR}/eigen")
    endif()
    
    # fmt
    if(NOT TARGET fmt::fmt)
        add_library(fmt::fmt INTERFACE IMPORTED)
        set_target_properties(fmt::fmt PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${MESHLIB_THIRDPARTY_DIR}/fmt")
    endif()
    
    # spdlog
    if(NOT TARGET spdlog::spdlog)
        add_library(spdlog::spdlog INTERFACE IMPORTED)
        set_target_properties(spdlog::spdlog PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${MESHLIB_THIRDPARTY_DIR}/spdlog")
    endif()
    
    # JsonCpp
    if(NOT TARGET JsonCpp::JsonCpp)
        add_library(JsonCpp::JsonCpp INTERFACE IMPORTED)
        set_target_properties(JsonCpp::JsonCpp PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${MESHLIB_THIRDPARTY_DIR}/jsoncpp")
    endif()
    
    # tl-expected
    if(NOT TARGET tl::expected)
        add_library(tl::expected INTERFACE IMPORTED)
        set_target_properties(tl::expected PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${MESHLIB_THIRDPARTY_DIR}/expected")
    endif()

    # TBB Shim (Header-only)
    if(NOT TARGET TBB::tbb)
        add_library(TBB::tbb INTERFACE IMPORTED)
        set_target_properties(TBB::tbb PROPERTIES 
            INTERFACE_INCLUDE_DIRECTORIES "${MESHLIB_THIRDPARTY_DIR}"
        )
    endif()

    # libzip dummy
    if(NOT TARGET libzip::zip)
        add_library(libzip::zip INTERFACE IMPORTED)
    endif()

    # ZLIB dummy
    if(NOT TARGET ZLIB::ZLIB)
        add_library(ZLIB::ZLIB INTERFACE IMPORTED)
    endif()

    # MbedTLS dummy
    if(NOT TARGET MbedTLS::mbedtls)
        add_library(MbedTLS::mbedtls INTERFACE IMPORTED)
    endif()

    # bit_cast shim
    include_directories("${MESHLIB_THIRDPARTY_DIR}")
endif()
