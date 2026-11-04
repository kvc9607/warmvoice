# Windows only: the APO DLL (loaded by audiodg.exe) and the installer tool.
add_library(WarmVoiceApo SHARED WarmVoiceApo.cpp WarmVoiceApo.def)
target_link_libraries(WarmVoiceApo PRIVATE wvcore ole32 oleaut32 uuid propsys advapi32)
target_compile_definitions(WarmVoiceApo PRIVATE UNICODE _UNICODE)
target_include_directories(WarmVoiceApo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(MSVC)
  # Static runtime: audiodg.exe must not depend on a VC++ redistributable being installed.
  set_property(TARGET WarmVoiceApo PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

add_executable(wvinstall wvinstall.cpp)
target_link_libraries(wvinstall PRIVATE ole32 oleaut32 uuid propsys advapi32)
target_compile_definitions(wvinstall PRIVATE UNICODE _UNICODE)
target_include_directories(wvinstall PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/core/include)
if(MSVC)
  set_property(TARGET wvinstall PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
  # ask for administrator rights when started
  set_property(TARGET wvinstall APPEND PROPERTY LINK_FLAGS "/MANIFESTUAC:\"level='requireAdministrator' uiAccess='false'\"")
endif()
