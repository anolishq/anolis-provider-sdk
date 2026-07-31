add_executable(provider_sdk_unit_tests
    proto_smoke_test.cpp
    device_model_test.cpp
    transport_test.cpp
    logging_test.cpp
    spine_test.cpp
    device_handlers_test.cpp
    i2c_test.cpp
)

target_link_libraries(provider_sdk_unit_tests PRIVATE
    anolis::provider_sdk
    anolis::provider_sdk_i2c
    GTest::gtest_main
)

if(ANOLIS_PROVIDER_SDK_ENABLE_CONFIG)
    target_sources(provider_sdk_unit_tests PRIVATE
        config_schema_test.cpp
        config_validate_test.cpp
    )
    target_link_libraries(provider_sdk_unit_tests PRIVATE anolis::provider_sdk_config)
endif()

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
