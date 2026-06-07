add_library(citadel_core STATIC
    core/mana/ManaCostShard.cpp
    core/mana/ManaCost.cpp
    core/card/CardType.cpp
    core/card/CardRules.cpp
    core/db/CardScriptParser.cpp
    core/db/CardDb.cpp
    core/db/EditionDb.cpp
    game/ManaPool.cpp
    game/GameState.cpp
    game/CardFilter.cpp
    game/LayerEngine.cpp
    game/DeckLoader.cpp
    game/TriggerSystem.cpp
    game/KeywordAbility.cpp
    game/StateBasedActions.cpp
    game/TurnManager.cpp
    game/ability/ScriptLine.cpp
    game/ability/Effects.cpp
    game/ability/AbilityProcessor.cpp
    game/ai/AiPlayer.cpp
    game/ai/ComboDatabase.cpp
    game/ai/MctsSearch.cpp
    game/ai/SaltDatabase.cpp
    game/ai/EdhrecClient.cpp
    game/ai/ValueNetInference.cpp
    sim/GameRunner.cpp
)

target_include_directories(citadel_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(citadel_core PUBLIC cxx_std_20)
target_link_libraries(citadel_core PRIVATE miniz nlohmann_json::nlohmann_json
    $<$<PLATFORM_ID:Windows>:winhttp>
)

if(MSVC)
    target_compile_options(citadel_core PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(citadel_core PRIVATE -Wall -Wextra -Werror)
endif()

add_executable(mtg
    main.cpp
    ui/CardRenderer.cpp
    ui/BoardRenderer.cpp
    ui/CardImageDownloader.cpp
    ui/CardDownloadScreen.cpp
    ui/HumanController.cpp
    ui/MainMenuScreen.cpp
    ui/MatchSetupScreen.cpp
    ui/DeckEditorScreen.cpp
    ui/GameWindow.cpp
    ui/SoundManager.cpp
)
target_link_libraries(mtg PRIVATE citadel_core sfml-graphics sfml-window sfml-system sfml-audio nlohmann_json::nlohmann_json)

if(WIN32)
    target_link_libraries(mtg PRIVATE winhttp)
    target_link_options(mtg PRIVATE /STACK:8388608)
endif()

if(WIN32)
    add_custom_command(TARGET mtg POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:mtg>
            $<TARGET_FILE_DIR:mtg>
        COMMAND_EXPAND_LISTS
    )
    add_custom_command(TARGET mtg POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${sfml_SOURCE_DIR}/extlibs/bin/x64/openal32.dll"
            $<TARGET_FILE_DIR:mtg>
    )
endif()

add_custom_command(TARGET mtg POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/data
        $<TARGET_FILE_DIR:mtg>/data
)

if(EXISTS ${CMAKE_SOURCE_DIR}/res)
    add_custom_command(TARGET mtg POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
            ${CMAKE_SOURCE_DIR}/res
            $<TARGET_FILE_DIR:mtg>/res
    )
endif()

if(CITADEL_SIM_ENABLED)
    add_subdirectory(python)
endif()

option(ENABLE_ASAN "Enable AddressSanitizer for memory error detection" OFF)
if(ENABLE_ASAN)
    if(MSVC)
        target_compile_options(citadel_core PRIVATE /fsanitize=address)
        target_compile_options(mtg        PRIVATE /fsanitize=address)
        target_compile_options(citadel_tests PRIVATE /fsanitize=address)
    else()
        target_compile_options(citadel_core PRIVATE -fsanitize=address -fno-omit-frame-pointer)
        target_link_options   (citadel_core PUBLIC  -fsanitize=address)
        target_compile_options(mtg        PRIVATE -fsanitize=address -fno-omit-frame-pointer)
        target_link_options   (mtg        PUBLIC  -fsanitize=address)
        target_compile_options(citadel_tests PRIVATE -fsanitize=address -fno-omit-frame-pointer)
        target_link_options   (citadel_tests PUBLIC  -fsanitize=address)
    endif()
    message(STATUS "AddressSanitizer enabled. Build and run citadel_tests to check for leaks.")
endif()

enable_testing()

add_executable(citadel_tests
    test/test_interactions.cpp
)
target_link_libraries(citadel_tests PRIVATE citadel_core)
target_compile_features(citadel_tests PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(citadel_tests PRIVATE /W3 /utf-8)
endif()
add_test(NAME citadel_interactions COMMAND citadel_tests)
