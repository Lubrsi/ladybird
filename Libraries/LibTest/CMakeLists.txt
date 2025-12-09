include(mimalloc)

add_library(LibTestMain OBJECT TestMain.cpp AssertionHandler.cpp)
target_link_libraries(LibTestMain PUBLIC GenericClangPlugin)
target_link_libraries(LibTestMain PRIVATE mimalloc-static)

add_library(JavaScriptTestRunnerMain OBJECT JavaScriptTestRunnerMain.cpp)
target_link_libraries(JavaScriptTestRunnerMain PRIVATE mimalloc-static)

set(SOURCES
    TestSuite.cpp
)

ladybird_lib(LibTest test)
target_link_libraries(LibTest PRIVATE AK LibCore LibFileSystem)
set_target_properties(LibTest PROPERTIES OUTPUT_NAME lagom-test)
