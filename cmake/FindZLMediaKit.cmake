# cmake/FindZLMediaKit.cmake
# 自动读取顶层自动设置好的 TARGET_PLATFORM
if(NOT DEFINED TARGET_PLATFORM)
    message(FATAL_ERROR "TARGET_PLATFORM not defined!")
endif()
set(ZLM_ROOT "${PROJECT_SOURCE_DIR}/install/${TARGET_PLATFORM}")
message(STATUS "Search ZLMediaKit at: ${ZLM_ROOT}")

# 查找头文件，禁止系统路径
find_path(ZLM_HEADER
    NAMES ZLMediaKit/mk_common.h
    HINTS ${ZLM_ROOT}/include
    NO_DEFAULT_PATH
)

# ZLMediaKit 核心C API库：名字 mk_api
find_library(ZLM_MEDIAKIT_LIB
    NAMES mk_api
    HINTS ${ZLM_ROOT}/lib
    NO_DEFAULT_PATH
)

# 校验
if(NOT ZLM_HEADER OR NOT ZLM_MEDIAKIT_LIB)
    message(FATAL_ERROR
        "ZLMediaKit for platform [${TARGET_PLATFORM}] NOT FOUND!\n"
        "Search path: ${ZLM_ROOT}\n"
        "Check install/${TARGET_PLATFORM}/include and lib\n"
        "Expect library name: libmk_api.so"
    )
endif()

set(ZLM_INCLUDE_DIRS ${ZLM_ROOT}/include)
set(ZLM_LIBRARIES ${ZLM_MEDIAKIT_LIB})
set(ZLM_LIB_DIR ${ZLM_ROOT}/lib)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(ZLMediaKit DEFAULT_MSG ZLM_INCLUDE_DIRS ZLM_LIBRARIES)
mark_as_advanced(ZLM_INCLUDE_DIRS ZLM_LIBRARIES ZLM_LIB_DIR)
