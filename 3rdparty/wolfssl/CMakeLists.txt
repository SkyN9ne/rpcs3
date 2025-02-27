if(USE_SYSTEM_WOLFSSL)
	message("-- RPCS3: using shared wolfssl")
	pkg_check_modules(WolfSSL REQUIRED IMPORTED_TARGET wolfssl>=4.7.0)
	add_library(wolfssl INTERFACE)
	target_link_libraries(wolfssl INTERFACE PkgConfig::WolfSSL)
else()
	# TODO(cjj19970505@live.cn)
	# OPENSSL_EXTRA, WOLFSSL_DES_ECB and HAVE_SNI are unconfigurable from CMake cache.
	# but they do have it in a TODO list (wolfssl/CMakeList, 1021)
	add_compile_definitions(OPENSSL_EXTRA WOLFSSL_DES_ECB HAVE_SNI HAVE_WRITE_DUP)

	set(WOLFSSL_TLS13 "no" CACHE INTERNAL "")
	set(WOLFSSL_SHA224 "yes" CACHE INTERNAL "")
	set(WOLFSSL_SHA3 "yes" CACHE INTERNAL "")
	set(WOLFSSL_SHAKE256 "yes" CACHE INTERNAL "")
	set(WOLFSSL_BASE64_ENCODE "no" CACHE INTERNAL "")
	set(WOLFSSL_DES3 "yes" CACHE INTERNAL "")
	set(WOLFSSL_POLY1305 "yes" CACHE INTERNAL "")
	set(WOLFSSL_CHACHA "yes" CACHE INTERNAL "")
	set(WOLFSSL_FILESYSTEM "yes" CACHE INTERNAL "")
	set(WOLFSSL_PWDBASED "yes" CACHE INTERNAL "")
	set(WOLFSSL_FAST_MATH "no" CACHE INTERNAL "")
	set(WOLFSSL_EXAMPLES "no" CACHE INTERNAL "")
	set(WOLFSSL_CRYPT_TESTS "no" CACHE INTERNAL "")
	set(WOLFSSL_ASYNC_THREADS "no" CACHE INTERNAL "")
	set(WOLFSSL_CONFIG_H "no" CACHE INTERNAL "")

	add_subdirectory(wolfssl EXCLUDE_FROM_ALL)

	set(WolfSSL_LIBRARY "$<TARGET_FILE:wolfssl>" CACHE INTERNAL "")

	# "${CMAKE_CURRENT_SOURCE_DIR}/wolfssl/wolfssl/" provides openssl headers
	# So that curl can be built on an environment where openssl headers are not provided
	set(WolfSSL_INCLUDE_DIR
		"${CMAKE_CURRENT_SOURCE_DIR}/wolfssl/"
		"${CMAKE_CURRENT_SOURCE_DIR}/wolfssl/wolfssl/"
		"${CMAKE_CURRENT_BINARY_DIR}/wolfssl/"
		CACHE INTERNAL "")
endif()
