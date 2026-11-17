ladybird_test_suite(LibDatabase)

set(TEST_SOURCES
    TestDatabase.cpp
    TestMigrations.cpp
)

target_sources(TestLibDatabase PRIVATE ${TEST_SOURCES})
target_link_libraries(TestLibDatabase PRIVATE LibCore LibDatabase LibFileSystem)

if (CMAKE_VERSION VERSION_GREATER_EQUAL 4.3.1)
    target_link_libraries(TestLibDatabase PRIVATE SQLite3::SQLite3)
else()
    target_link_libraries(TestLibDatabase PRIVATE SQLite::SQLite3)
endif()
