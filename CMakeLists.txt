cmake_minimum_required(VERSION 4.2)

set(NAME "ROCKBackpack")
set(FRIENDLY_NAME "ROCK Backpack")
set(VERSION 0.1.0)

message(">>> Start build for '${FRIENDLY_NAME}' v${VERSION}")

set(ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(SOURCE_DIR "${ROOT_DIR}/src")
get_filename_component(WORKSPACE_ROOT "${ROOT_DIR}/../../PROJECT_ROCK_V2" ABSOLUTE)
set(SPRIGGIT_PROJECT_DIR "${ROOT_DIR}/plugin/ROCK_Backpack_spriggit")
set(GENERATED_DATA_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated-data")
set(GENERATED_PLUGIN "${GENERATED_DATA_DIR}/ROCK_Backpack.esp")
set(BUNDLED_BACKPACK_ASSET_ROOT "${ROOT_DIR}/assets/runtime")
set(BUNDLED_BACKPACK_PROVENANCE
  "${ROOT_DIR}/assets/provenance/military_backpack_02.json"
)
set(BUNDLED_BACKPACK_FILES
  "${BUNDLED_BACKPACK_ASSET_ROOT}/Meshes/military_backapack/military_backpack_02.nif"
  "${BUNDLED_BACKPACK_ASSET_ROOT}/Meshes/military_backapack/military_backpack_02_go.nif"
  "${BUNDLED_BACKPACK_ASSET_ROOT}/Materials/military_backapack/military_backpack_02.bgsm"
  "${BUNDLED_BACKPACK_ASSET_ROOT}/Textures/military_backapack/military_backpack_02_d.dds"
  "${BUNDLED_BACKPACK_ASSET_ROOT}/Textures/military_backapack/military_backpack_02_n.dds"
  "${BUNDLED_BACKPACK_ASSET_ROOT}/Textures/military_backapack/military_backpack_02_s.dds"
  "${BUNDLED_BACKPACK_ASSET_ROOT}/Textures/military_backapack/military_backpack_02_m.dds"
)

foreach(bundled_file IN LISTS BUNDLED_BACKPACK_FILES BUNDLED_BACKPACK_PROVENANCE)
  if(NOT EXISTS "${bundled_file}")
    message(FATAL_ERROR "Required bundled Military Backpack 2 file is missing: '${bundled_file}'")
  endif()
endforeach()

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
  message(FATAL_ERROR "In-source builds are not allowed")
endif()

macro(set_from_environment VARIABLE)
  if(NOT DEFINED ${VARIABLE} AND DEFINED ENV{${VARIABLE}})
    set(${VARIABLE} $ENV{${VARIABLE}})
  endif()
endmacro()

set_from_environment(VCPKG_ROOT)
if(DEFINED VCPKG_ROOT)
  set(VCPKG_ROOT "${VCPKG_ROOT}" CACHE PATH "Local vcpkg checkout")
  set(CMAKE_TOOLCHAIN_FILE "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
  set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "")
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "")
else()
  message(FATAL_ERROR "VCPKG_ROOT is not set")
endif()

if(DEFINED F4VR_COMMON_FRAMEWORK_PATH AND NOT F4VR_COMMON_FRAMEWORK_PATH STREQUAL "")
  set(F4VR_COMMON_FRAMEWORK_PATH_SELECTED "${F4VR_COMMON_FRAMEWORK_PATH}" CACHE STRING "" FORCE)
else()
  set(F4VR_COMMON_FRAMEWORK_PATH_SELECTED "${WORKSPACE_ROOT}/libraries_and_tools/F4VR-CommonFramework" CACHE STRING "" FORCE)
endif()
if(NOT EXISTS "${F4VR_COMMON_FRAMEWORK_PATH_SELECTED}/src/ModBase.h")
  message(FATAL_ERROR "F4VR-CommonFramework path not found in '${F4VR_COMMON_FRAMEWORK_PATH_SELECTED}'")
endif()

if(DEFINED COMMON_LIB_F4VR_PATH AND NOT COMMON_LIB_F4VR_PATH STREQUAL "")
  set(COMMON_LIB_F4VR_PATH_SELECTED "${COMMON_LIB_F4VR_PATH}" CACHE STRING "" FORCE)
else()
  set(COMMON_LIB_F4VR_PATH_SELECTED "${WORKSPACE_ROOT}/libraries_and_tools/CommonLibF4VR" CACHE STRING "" FORCE)
endif()
if(NOT EXISTS "${COMMON_LIB_F4VR_PATH_SELECTED}/CommonLibF4/include/REL/Relocation.h")
  message(FATAL_ERROR "CommonLibF4VR path not found in '${COMMON_LIB_F4VR_PATH_SELECTED}'")
endif()
set(COMMON_LIB_F4VR_PATH "${COMMON_LIB_F4VR_PATH_SELECTED}" CACHE STRING "" FORCE)

set(ROCK_INCLUDE_DIR "${WORKSPACE_ROOT}/main_projects/ROCK/include" CACHE PATH "ROCK public include directory")
if(NOT EXISTS "${ROCK_INCLUDE_DIR}/ROCK/Client.h")
  message(FATAL_ERROR "ROCK_INCLUDE_DIR does not contain ROCK/Client.h: ${ROCK_INCLUDE_DIR}")
endif()

set(SPRIGGIT_CLI_PATH_SELECTED "${SPRIGGIT_CLI_PATH}" CACHE FILEPATH "" FORCE)
if(SPRIGGIT_CLI_PATH_SELECTED STREQUAL "" OR NOT EXISTS "${SPRIGGIT_CLI_PATH_SELECTED}")
  message(FATAL_ERROR "Spriggit CLI not found at '${SPRIGGIT_CLI_PATH_SELECTED}'")
endif()

project(${NAME} VERSION ${VERSION} LANGUAGES CXX RC)
set(RUNTIME_TARGET "${PROJECT_NAME}Runtime")
set(RPSUI_INCLUDE_DIR "${WORKSPACE_ROOT}/../ui_mods/RPS_UI_Framework/include" CACHE PATH "RPS UI Framework public include directory")
# RPS Framework's staged package: the settings file (RPS::Settings) and the
# header-only hook chaining (RPS/Runtime/HookChain.h).
set(RPSFramework_DIR "${WORKSPACE_ROOT}/my_frameworks_and_sdks/RPS_Framework/build-fast/stage/lib/cmake/RPSFramework"
  CACHE PATH "RPS Framework staged package")
find_package(RPSFramework CONFIG REQUIRED)
# Backpack shares PALM's icon names (RobCoPALM/Icons.h) and never calls PALM.
set(PALM_INCLUDE_DIR "${WORKSPACE_ROOT}/../ui_mods/ROCK_Whell_Menu/include" CACHE PATH "RobCo PALM public include directory")
set(IMGUI_PATH "${WORKSPACE_ROOT}/../devtools/imgui" CACHE PATH "Local Dear ImGui source")
if(NOT EXISTS "${RPSUI_INCLUDE_DIR}/RPS_UI_Framework/Cooperation.h" OR NOT EXISTS "${PALM_INCLUDE_DIR}/RobCoPALM/Icons.h" OR NOT EXISTS "${IMGUI_PATH}/imgui.cpp")
  message(FATAL_ERROR "RPS UI Framework headers, RobCo PALM headers or local Dear ImGui source is missing")
endif()
include_directories("${PALM_INCLUDE_DIR}")
# RPS UI owns the in-game panels' ImGui contexts and renders them. This copy
# of Dear ImGui only issues calls into those contexts, so it is built with RPS
# UI's shared configuration and without a renderer backend.
add_library(backpack_imgui STATIC "${IMGUI_PATH}/imgui.cpp" "${IMGUI_PATH}/imgui_draw.cpp"
  "${IMGUI_PATH}/imgui_tables.cpp" "${IMGUI_PATH}/imgui_widgets.cpp")
# RPS UI's support headers: its built-in theme and how a theme becomes ImGui's style.
get_filename_component(RPSUI_SUPPORT_DIR "${RPSUI_INCLUDE_DIR}/../support" ABSOLUTE)
target_include_directories(backpack_imgui PUBLIC "${IMGUI_PATH}" "${RPSUI_INCLUDE_DIR}" "${RPSUI_SUPPORT_DIR}")
target_compile_features(backpack_imgui PUBLIC cxx_std_23)
target_compile_definitions(backpack_imgui PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN IMGUI_USER_CONFIG="RPS_UI_Framework/ImGuiBuild.h")
target_link_libraries(backpack_imgui PUBLIC d3d11 dxgi d3dcompiler)
# The desktop preview renders its own window.
add_library(backpack_imgui_dx11 STATIC "${IMGUI_PATH}/backends/imgui_impl_dx11.cpp")
target_include_directories(backpack_imgui_dx11 PUBLIC "${IMGUI_PATH}/backends")
target_link_libraries(backpack_imgui_dx11 PUBLIC backpack_imgui)
find_package(nlohmann_json CONFIG REQUIRED)
add_library(backpack_view STATIC src/ui/PanelView.cpp src/ui/Theme.cpp src/ui/IconAtlas.cpp src/ui/ItemImage.cpp src/ui/PreviewAlphaPass.cpp src/ui/PreviewInventory.cpp)
target_include_directories(backpack_view PUBLIC src src/ui)
target_link_libraries(backpack_view PUBLIC backpack_imgui windowscodecs ole32 shell32)
target_link_libraries(backpack_view PRIVATE nlohmann_json::nlohmann_json)
target_compile_features(backpack_view PUBLIC cxx_std_23)
target_compile_options(backpack_view PRIVATE /utf-8)
set(BACKPACK_PREVIEW_DIRECTORY "" CACHE PATH "Override native/desktop item-image capture directory for local development")
if(BACKPACK_PREVIEW_DIRECTORY)
  target_compile_definitions(backpack_view PRIVATE ROCK_BACKPACK_PREVIEW_DIRECTORY="${BACKPACK_PREVIEW_DIRECTORY}")
endif()

add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION OFF)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_DEBUG OFF)

# backpack_imgui above is the ImGui that issues calls into RPS UI's contexts; a
# second copy from the framework's ImGui layer would collide at link time. The
# framework's Tracy configuration downloads Tracy at configure time and is unused.
set(F4CF_WITH_IMGUI_UI OFF CACHE BOOL "" FORCE)
set(F4CF_WITH_TRACY OFF CACHE BOOL "" FORCE)
add_subdirectory(${F4VR_COMMON_FRAMEWORK_PATH_SELECTED} "F4VR-CommonFramework" EXCLUDE_FROM_ALL)

file(GLOB_RECURSE headers CONFIGURE_DEPENDS "${SOURCE_DIR}/*.h")
file(GLOB_RECURSE sources CONFIGURE_DEPENDS "${SOURCE_DIR}/*.cpp")
list(FILTER sources EXCLUDE REGEX "/ui/")
set_source_files_properties(src/RockServices.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
file(GLOB_RECURSE spriggit_sources CONFIGURE_DEPENDS "${SPRIGGIT_PROJECT_DIR}/*")
set(deployment_sources ${BUNDLED_BACKPACK_FILES})
source_group(TREE ${ROOT_DIR} FILES
  ${headers}
  ${sources}
  ${spriggit_sources}
  ${deployment_sources}
  ${BUNDLED_BACKPACK_PROVENANCE}
)

add_custom_command(
  OUTPUT "${GENERATED_PLUGIN}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_DATA_DIR}"
  COMMAND "${SPRIGGIT_CLI_PATH_SELECTED}" deserialize
          -i "${SPRIGGIT_PROJECT_DIR}"
          -o "${GENERATED_PLUGIN}"
          -p Spriggit.Yaml.Fallout4
          -v 0.40
          -b 0
  DEPENDS ${spriggit_sources}
  COMMENT "Generating ROCK_Backpack.esp from Spriggit source"
  VERBATIM
)
add_custom_target(ROCKBackpackPlugin DEPENDS "${GENERATED_PLUGIN}")

find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(BUNDLED_BACKPACK_VALIDATOR "${ROOT_DIR}/tests/ValidateBundledBackpack.py")
add_custom_target(ROCKBackpackBundledAssetRegression
  COMMAND "${Python3_EXECUTABLE}"
          "${BUNDLED_BACKPACK_VALIDATOR}"
          --source-root "${ROOT_DIR}"
          --plugin "${GENERATED_PLUGIN}"
  DEPENDS
    ${BUNDLED_BACKPACK_FILES}
    ${BUNDLED_BACKPACK_PROVENANCE}
    "${BUNDLED_BACKPACK_VALIDATOR}"
  COMMENT "Validating bundled Military Backpack 2 NIF, material, texture, and plugin contracts"
  VERBATIM
)
add_dependencies(ROCKBackpackBundledAssetRegression ROCKBackpackPlugin)

add_library(${RUNTIME_TARGET} SHARED ${headers} ${sources} src/ui/IconResources.rc)
add_dependencies(${RUNTIME_TARGET} ROCKBackpackPlugin ROCKBackpackBundledAssetRegression)
set_target_properties(${RUNTIME_TARGET} PROPERTIES OUTPUT_NAME ${PROJECT_NAME})

target_compile_features(${RUNTIME_TARGET} PRIVATE cxx_std_23)
target_compile_definitions(${RUNTIME_TARGET} PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX NOMMNOSOUND)
target_include_directories(${RUNTIME_TARGET}
  PRIVATE
    ${SOURCE_DIR}
    ${ROCK_INCLUDE_DIR}
    ${RPSUI_INCLUDE_DIR}
    "${WORKSPACE_ROOT}/secondary_projects/VR_Vanilla_Fixes/include"
)
set_property(TARGET ${RUNTIME_TARGET} PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE)

find_package(spdlog CONFIG REQUIRED)

target_link_libraries(${RUNTIME_TARGET}
  PRIVATE
    F4VRCommon::framework
    RPS::Settings
    backpack_view
    spdlog::spdlog
    d3d11.lib
    dxgi.lib
    d3dcompiler.lib
    shell32.lib
    version.lib
    bcrypt.lib
)

target_precompile_headers(${RUNTIME_TARGET} PRIVATE src/PCH.h)

target_compile_options(${RUNTIME_TARGET}
  PRIVATE
    /sdl
    /utf-8
    /Zi
    /permissive-
    /Zc:alignedNew
    /Zc:auto
    /Zc:char8_t
    /Zc:__cplusplus
    /Zc:externC
    /Zc:externConstexpr
    /Zc:forScope
    /Zc:hiddenFriend
    /Zc:implicitNoexcept
    /Zc:lambda
    /Zc:noexceptTypes
    /Zc:preprocessor
    /Zc:referenceBinding
    /Zc:rvalueCast
    /Zc:sizedDealloc
    /Zc:strictStrings
    /Zc:ternary
    /Zc:threadSafeInit
    /Zc:tlsGuards
    /Zc:trigraphs
    /Zc:wchar_t
    /external:anglebrackets
    /external:W0
    /W4
    /wd4715
    "$<$<CONFIG:RELEASE>:/Zc:inline;/JMC-;/Ob3>"
)

target_link_options(${RUNTIME_TARGET}
  PRIVATE
    /DEF:${SOURCE_DIR}/exports.def
    "$<$<CONFIG:DEBUG>:/INCREMENTAL;/OPT:NOREF;/OPT:NOICF>"
    "$<$<CONFIG:RELEASE>:/INCREMENTAL:NO;/OPT:REF;/OPT:ICF;/DEBUG:FULL>"
)

string(REPLACE "/Ob2" "/Ob3" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")

option(POST_BUILD_COPY_PLUGIN "Copy ROCK Backpack runtime, plugin data, and bundled backpack assets after building." OFF)
set(COPY_PLUGIN_BASE_PATH "" CACHE STRING "Semicolon-separated mod roots.")

# The public build target is intentionally always out of date. Its copy_if_different
# steps make the custom-fast target content-aware without forcing the DLL to relink
# when only Spriggit data or backpack assets change. The native UI is in the DLL.
add_custom_target(${PROJECT_NAME} ALL
  DEPENDS ${RUNTIME_TARGET} ROCKBackpackPlugin ${deployment_sources}
)

if(POST_BUILD_COPY_PLUGIN)
  if(DEFINED COPY_PLUGIN_BASE_PATH AND NOT COPY_PLUGIN_BASE_PATH STREQUAL "")
    set(COPY_PLUGIN_PATH_LIST ${COPY_PLUGIN_BASE_PATH})
  else()
    message(FATAL_ERROR "COPY_PLUGIN_BASE_PATH is required when POST_BUILD_COPY_PLUGIN is enabled")
  endif()

  message(">>> Create post build command: copy targets to:")
  foreach(copy_path IN LISTS COPY_PLUGIN_PATH_LIST)
    message("    ${copy_path}")
    add_custom_command(
      TARGET ${PROJECT_NAME}
      POST_BUILD
      COMMENT "Deploying ROCK Backpack to '${copy_path}'"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${copy_path}/F4SE/Plugins"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:${RUNTIME_TARGET}> "${copy_path}/F4SE/Plugins/"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_PDB_FILE:${RUNTIME_TARGET}> "${copy_path}/F4SE/Plugins/"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${GENERATED_PLUGIN}" "${copy_path}/ROCK_Backpack.esp"
      COMMAND ${CMAKE_COMMAND} -E rm -f "${copy_path}/Meshes/ROCKBackpack/open-cube.nif"
      COMMAND ${CMAKE_COMMAND} -E copy_directory "${BUNDLED_BACKPACK_ASSET_ROOT}/Meshes" "${copy_path}/Meshes"
      COMMAND ${CMAKE_COMMAND} -E copy_directory "${BUNDLED_BACKPACK_ASSET_ROOT}/Materials" "${copy_path}/Materials"
      COMMAND ${CMAKE_COMMAND} -E copy_directory "${BUNDLED_BACKPACK_ASSET_ROOT}/Textures" "${copy_path}/Textures"
    )
  endforeach()
else()
  message(">>> No post build copy")
endif()

enable_testing()
add_executable(ROCKBackpackPreviewRecoveryTests tests/PreviewRecoveryTests.cpp)
target_include_directories(ROCKBackpackPreviewRecoveryTests PRIVATE src)
target_compile_features(ROCKBackpackPreviewRecoveryTests PRIVATE cxx_std_23)
add_test(NAME ROCKBackpackPreviewRecoveryTests COMMAND ROCKBackpackPreviewRecoveryTests)

add_executable(ROCKBackpackInventoryMirrorTests tests/InventoryMirrorTests.cpp)
target_include_directories(ROCKBackpackInventoryMirrorTests PRIVATE src)
target_compile_features(ROCKBackpackInventoryMirrorTests PRIVATE cxx_std_23)
add_test(NAME ROCKBackpackInventoryMirrorTests COMMAND ROCKBackpackInventoryMirrorTests)
add_executable(ROCKBackpackInventoryCapacityTests tests/InventoryCapacityTests.cpp)
target_include_directories(ROCKBackpackInventoryCapacityTests PRIVATE src "${ROCK_INCLUDE_DIR}")
target_compile_features(ROCKBackpackInventoryCapacityTests PRIVATE cxx_std_23)
add_test(NAME ROCKBackpackInventoryCapacityTests COMMAND ROCKBackpackInventoryCapacityTests)
add_executable(ROCKBackpackAuthoredPullTests tests/AuthoredWeaponPullTests.cpp)
target_include_directories(ROCKBackpackAuthoredPullTests PRIVATE src "${ROCK_INCLUDE_DIR}")
target_compile_features(ROCKBackpackAuthoredPullTests PRIVATE cxx_std_23)
add_test(NAME ROCKBackpackAuthoredPullTests COMMAND ROCKBackpackAuthoredPullTests)
add_executable(ROCKBackpackCollisionCloneTests tests/CollisionCloneTests.cpp)
target_include_directories(ROCKBackpackCollisionCloneTests PRIVATE src)
target_compile_features(ROCKBackpackCollisionCloneTests PRIVATE cxx_std_23)
add_test(NAME ROCKBackpackCollisionCloneTests COMMAND ROCKBackpackCollisionCloneTests)
add_executable(ROCKBackpackStoredReferenceRecoveryTests tests/StoredReferenceRecoveryTests.cpp)
target_include_directories(ROCKBackpackStoredReferenceRecoveryTests PRIVATE src)
target_compile_features(ROCKBackpackStoredReferenceRecoveryTests PRIVATE cxx_std_23)
add_test(NAME ROCKBackpackStoredReferenceRecoveryTests COMMAND ROCKBackpackStoredReferenceRecoveryTests)
add_executable(ROCKBackpackWeaponTransitionTests tests/WeaponTransitionTests.cpp)
target_include_directories(ROCKBackpackWeaponTransitionTests PRIVATE src)
target_compile_features(ROCKBackpackWeaponTransitionTests PRIVATE cxx_std_23)
add_test(NAME ROCKBackpackWeaponTransitionTests COMMAND ROCKBackpackWeaponTransitionTests)
add_executable(ROCKBackpackPolicyTests tests/PolicyTests.cpp)
target_compile_features(ROCKBackpackPolicyTests PRIVATE cxx_std_23)
target_include_directories(ROCKBackpackPolicyTests PRIVATE ${SOURCE_DIR})
target_link_libraries(ROCKBackpackPolicyTests PRIVATE RPS::Settings)
add_test(NAME ROCKBackpackPolicyTests COMMAND $<TARGET_FILE:ROCKBackpackPolicyTests>)
add_executable(ROCKBackpackGrabDispatchTests tests/GrabCommandDispatchTests.cpp)
target_include_directories(ROCKBackpackGrabDispatchTests PRIVATE src "${ROCK_INCLUDE_DIR}")
target_compile_features(ROCKBackpackGrabDispatchTests PRIVATE cxx_std_23)
add_test(NAME ROCKBackpackGrabDispatchTests COMMAND ROCKBackpackGrabDispatchTests)
add_executable(ROCKBackpackDropPlacementTests tests/DropPlacementTests.cpp)
target_include_directories(ROCKBackpackDropPlacementTests PRIVATE src)
target_compile_features(ROCKBackpackDropPlacementTests PRIVATE cxx_std_23)
add_test(NAME ROCKBackpackDropPlacementTests COMMAND ROCKBackpackDropPlacementTests)

add_executable(ROCKBackpackStorageGridTests tests/StorageGridTests.cpp)
target_include_directories(ROCKBackpackStorageGridTests PRIVATE src)
target_compile_features(ROCKBackpackStorageGridTests PRIVATE cxx_std_23)
add_test(NAME ROCKBackpackStorageGridTests COMMAND ROCKBackpackStorageGridTests)
add_executable(ROCKBackpackMountPlacementTests tests/MountPlacementTests.cpp src/BackpackConfig.cpp)
target_include_directories(ROCKBackpackMountPlacementTests PRIVATE src)
target_compile_features(ROCKBackpackMountPlacementTests PRIVATE cxx_std_23)
target_compile_definitions(ROCKBackpackMountPlacementTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(ROCKBackpackMountPlacementTests PRIVATE RPS::Settings shell32 ole32 spdlog::spdlog)
add_test(NAME ROCKBackpackMountPlacementTests COMMAND ROCKBackpackMountPlacementTests "${CMAKE_CURRENT_BINARY_DIR}")
add_executable(ROCKBackpackPreviewInventoryTests tests/PreviewInventoryTests.cpp)
target_link_libraries(ROCKBackpackPreviewInventoryTests PRIVATE backpack_view)
target_compile_features(ROCKBackpackPreviewInventoryTests PRIVATE cxx_std_23)
add_test(NAME ROCKBackpackPreviewInventoryTests COMMAND ROCKBackpackPreviewInventoryTests "${CMAKE_CURRENT_BINARY_DIR}")

add_executable(ROCKBackpackItemImageTests tests/ItemImageTests.cpp)
target_link_libraries(ROCKBackpackItemImageTests PRIVATE backpack_view)
target_compile_features(ROCKBackpackItemImageTests PRIVATE cxx_std_23)
add_test(NAME ROCKBackpackItemImageTests COMMAND ROCKBackpackItemImageTests
  "${CMAKE_CURRENT_BINARY_DIR}/item-image-test.png")

find_package(xbyak CONFIG REQUIRED)
add_executable(ROCKBackpackCaptureThunkTests tests/NativePreviewCaptureThunkTests.cpp)
target_include_directories(ROCKBackpackCaptureThunkTests PRIVATE src)
target_compile_features(ROCKBackpackCaptureThunkTests PRIVATE cxx_std_23)
target_link_libraries(ROCKBackpackCaptureThunkTests PRIVATE xbyak::xbyak)
add_test(NAME ROCKBackpackCaptureThunkTests COMMAND ROCKBackpackCaptureThunkTests)

add_executable(ROCKBackpackPreview WIN32 preview/Main.cpp preview/Scene3D.cpp preview/Scene3DModel.cpp preview/OfflineScene.cpp src/BackpackConfig.cpp src/ui/IconResources.rc
  "${IMGUI_PATH}/backends/imgui_impl_win32.cpp")
target_link_libraries(ROCKBackpackPreview PRIVATE backpack_view backpack_imgui_dx11 RPS::Settings shell32 spdlog::spdlog)
target_link_libraries(ROCKBackpackPreview PRIVATE nlohmann_json::nlohmann_json)
add_custom_command(TARGET ROCKBackpackPreview POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${ROOT_DIR}/preview/assets" "$<TARGET_FILE_DIR:ROCKBackpackPreview>/PreviewAssets")
target_compile_definitions(ROCKBackpackPreview PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(ROCKBackpackPreview PRIVATE /utf-8)
add_dependencies(ROCKBackpack ROCKBackpackPreview)
add_test(NAME ROCKBackpackPreviewButtonClick COMMAND ROCKBackpackPreview --exercise --sample --front
  --capture "${CMAKE_CURRENT_BINARY_DIR}/preview-button-click.png")
add_test(NAME ROCKBackpackPreviewCapture COMMAND ROCKBackpackPreview
  --capture "${CMAKE_CURRENT_BINARY_DIR}/backpack-preview.png")
add_test(NAME ROCKBackpackPullPromptCapture COMMAND ROCKBackpackPreview --pull-prompt-preview
  --capture "${CMAKE_CURRENT_BINARY_DIR}/pull-prompt-preview.png")
add_test(NAME ROCKBackpackPreviewOverflowDrag COMMAND ROCKBackpackPreview --exercise-overflow --sample
    --capture "${CMAKE_CURRENT_BINARY_DIR}/overflow-drag.png")
add_test(NAME ROCKBackpackPreviewMiscClick COMMAND ROCKBackpackPreview --exercise-misc --sample
  --capture "${CMAKE_CURRENT_BINARY_DIR}/preview-misc-click.png")

add_test(
  NAME ROCKBackpackBundledAssetTests
  COMMAND "${Python3_EXECUTABLE}"
          "${BUNDLED_BACKPACK_VALIDATOR}"
          --source-root "${ROOT_DIR}"
          --plugin "${GENERATED_PLUGIN}"
)
set_tests_properties(ROCKBackpackBundledAssetTests PROPERTIES LABELS "assets")

# Tests share build-fast but stay out of the default build, so building and deploying the
# plugin never compiles them. Build them with `--target build_tests`, then run ctest.
function(fo4dev_collect_targets directory result)
  get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
  get_property(children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
  foreach(child IN LISTS children)
    cmake_path(IS_PREFIX CMAKE_SOURCE_DIR "${child}" NORMALIZE child_is_ours)
    if(child_is_ours)
      fo4dev_collect_targets("${child}" child_targets)
      list(APPEND targets ${child_targets})
    endif()
  endforeach()
  set(${result} ${targets} PARENT_SCOPE)
endfunction()
fo4dev_collect_targets("${CMAKE_SOURCE_DIR}" fo4dev_targets)
add_custom_target(build_tests)
foreach(fo4dev_target IN LISTS fo4dev_targets)
  get_target_property(fo4dev_target_type ${fo4dev_target} TYPE)
  string(TOLOWER "${fo4dev_target}" fo4dev_target_name)
  if(fo4dev_target_name MATCHES "test" AND fo4dev_target_type MATCHES "^(EXECUTABLE|STATIC_LIBRARY|OBJECT_LIBRARY)$")
    set_target_properties(${fo4dev_target} PROPERTIES EXCLUDE_FROM_ALL TRUE)
    if(fo4dev_target_type STREQUAL "EXECUTABLE")
      add_dependencies(build_tests ${fo4dev_target})
    endif()
  endif()
endforeach()
