add_executable(MobileProcessFixture MobileProcessFixture.cpp)
target_compile_features(MobileProcessFixture PRIVATE cxx_std_20)
if(MINGW)
  target_link_options(MobileProcessFixture PRIVATE -municode)
endif()

add_neverd_unittest(NeverDMobileTests
  MobileCommonTests.cpp
  MobileCLITests.cpp
  MobileAndroidTests.cpp
  MobileDalvikReaderTests.cpp
  MobileDalvikSignatureTests.cpp
  MobileDalvikJavaTests.cpp
  MobileIOSTests.cpp
  MobileSwiftDemangleTests.cpp
  MobileProcessTests.cpp
  LINK_LIBS NeverDMobile
)
target_compile_definitions(NeverDMobileTests PRIVATE
  NEVERD_MOBILE_PROCESS_FIXTURE="$<TARGET_FILE:MobileProcessFixture>"
  NEVERD_MOBILE_CLI="$<TARGET_FILE:neverd>"
  NEVERD_MOBILE_FIXTURES="${CMAKE_SOURCE_DIR}/scripts/tests/fixtures/mobile")
add_dependencies(NeverDMobileTests MobileProcessFixture neverd)
