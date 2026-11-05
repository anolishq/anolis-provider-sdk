# anolis-provider-reference: the SDK's conformance fixture (CI runs the
# anolis-protocol harness against it) and a worked example of a provider.
add_executable(anolis-provider-reference reference_provider.cpp)
target_link_libraries(anolis-provider-reference PRIVATE anolis::provider_sdk anolis::provider_sdk_config)
anolis_provider_sdk_apply_warnings(anolis-provider-reference)

if(WIN32)
    target_compile_definitions(anolis-provider-reference PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
