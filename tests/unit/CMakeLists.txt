add_executable(provider_sdk_unit_tests
    proto_smoke_test.cpp
    device_model_test.cpp
    transport_test.cpp
    logging_test.cpp
)

target_link_libraries(provider_sdk_unit_tests PRIVATE
    anolis::provider_sdk
    GTest::gtest_main
)

anolis_provider_sdk_apply_warnings(provider_sdk_unit_tests)

if(WIN32)
    target_compile_definitions(provider_sdk_unit_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

include(GoogleTest)
gtest_discover_tests(provider_sdk_unit_tests
    TEST_PREFIX provider_sdk.unit.
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS "unit"
        TIMEOUT 30
)
