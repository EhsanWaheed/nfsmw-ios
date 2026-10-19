# pop_ios_bundle(<target>): an iPad app under build/ios, signed automatically by
# Xcode with RECOMP_IOS_TEAM, carrying the game files under game/.
function(pop_ios_bundle target)
  # The version LiveContainer / Settings show (v4: was a fixed 0.1) and the build number (commits in the checkout).
  if(NOT RECOMP_APP_VERSION)
    set(RECOMP_APP_VERSION "4.3")
  endif()
  execute_process(COMMAND git rev-list --count HEAD WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                  OUTPUT_VARIABLE RECOMP_APP_BUILD OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(NOT RECOMP_APP_BUILD)
    set(RECOMP_APP_BUILD "1")
  endif()
  configure_file(${POP_ROOT}/host/Info-ios.plist.in ${CMAKE_BINARY_DIR}/generated/Info-ios.plist @ONLY)
  set_target_properties(${target} PROPERTIES
    MACOSX_BUNDLE ON
    OUTPUT_NAME ${RECOMP_APP_NAME}
    RUNTIME_OUTPUT_DIRECTORY ${POP_BUILD_DIR}
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_BINARY_DIR}/generated/Info-ios.plist
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${RECOMP_BUNDLE_ID}
    XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${RECOMP_IOS_TEAM}"
    XCODE_ATTRIBUTE_CODE_SIGN_STYLE Automatic
    XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "Apple Development"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
    XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET 17.0
    XCODE_ATTRIBUTE_ENABLE_BITCODE NO
    "XCODE_ATTRIBUTE_INFOPLIST_KEY_UISupportedInterfaceOrientations~ipad"
      "UIInterfaceOrientationLandscapeLeft UIInterfaceOrientationLandscapeRight")
  if(RECOMP_VIDEO)
    # Let Xcode copy and sign the imported dylibs with the app's selected
    # identity/team. Plain post-build copies are not signed automatically.
    # These properties are supported since CMake 3.20 (the kit needs 3.24).
    set_target_properties(${target} PROPERTIES
      BUILD_WITH_INSTALL_RPATH ON
      INSTALL_RPATH "@executable_path/Frameworks"
      XCODE_ATTRIBUTE_LD_RUNPATH_SEARCH_PATHS "@executable_path/Frameworks"
      # Paths, not the imported target names: the Xcode generator resolves
      # embedded items by file, and the dylibs exist once the ffmpeg project
      # has built (the app depends on it through pop_link_video).
      XCODE_EMBED_FRAMEWORKS "${RECOMP_FFMPEG_LIBRARIES}"
      XCODE_EMBED_FRAMEWORKS_CODE_SIGN_ON_COPY YES)
    add_dependencies(${target} ffmpeg)
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS
      ${POP_ROOT}/third_party/ffmpeg/NOTICE.md)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${POP_ROOT}/third_party/ffmpeg/NOTICE.md
              $<TARGET_BUNDLE_CONTENT_DIR:${target}>/ffmpeg-NOTICE.md
      COMMENT "Copying the FFmpeg notice into ${RECOMP_APP_NAME}.app"
      VERBATIM)
  endif()
  # The game itself, filtered by games/<id>/game.toml [bundle].exclude. A stub
  # build has no game to bundle.
  if(NOT POP_TRANSLATE STREQUAL "STUB")
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${Python3_EXECUTABLE} ${POP_ROOT}/tools/stage_game_files.py
              --game-dir ${RECOMP_GAME_DIR}
              --source ${RECOMP_DEVELOPER_GAME_DIR}
              --dest $<TARGET_BUNDLE_CONTENT_DIR:${target}>/game
      WORKING_DIRECTORY ${POP_ROOT}
      COMMENT "Staging game files into ${RECOMP_APP_NAME}.app/game"
      VERBATIM)
    # The app icon is the game's own: the executable's icon group, scaled.
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${Python3_EXECUTABLE} ${POP_ROOT}/tools/extract_icon.py
              --exe ${RECOMP_DEVELOPER_EXE}
              --dest $<TARGET_BUNDLE_CONTENT_DIR:${target}>
              --image ${POP_BUILD_ROOT}/ios-icon.png
      WORKING_DIRECTORY ${POP_ROOT}
      COMMENT "Making the app icon (build/ios-icon.png when there is one, else the executable's icon)"
      VERBATIM)
    # v4.1: the launcher's cover art (launcher_ios.mm), the same image as the icon at full size
    if(EXISTS ${POP_BUILD_ROOT}/ios-icon.png)
      add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${POP_BUILD_ROOT}/ios-icon.png
                $<TARGET_BUNDLE_CONTENT_DIR:${target}>/LauncherArt.png
        VERBATIM)
    endif()
  endif()
  # classic-modes.json is one resource the host reads at startup; the
  # translation's symbol table is the other (the mod foundation's settings
  # store initialises only after it loads).
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${POP_ROOT}/tools/recomp/baseline/classic-modes.json
            $<TARGET_BUNDLE_CONTENT_DIR:${target}>/classic-modes.json
    VERBATIM)
  # The game's shipped on-screen control layouts, when it has any (spec
  # section 5): host_resource("controls") is the flat bundle's own root here.
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${Python3_EXECUTABLE} ${POP_ROOT}/tools/copy_layouts.py
            --game-dir ${RECOMP_GAME_DIR}
            --dest $<TARGET_BUNDLE_CONTENT_DIR:${target}>/controls
    WORKING_DIRECTORY ${POP_ROOT}
    COMMENT "Bundling ${RECOMP_APP_NAME}.app's control layouts"
    VERBATIM)
  # The General MIDI bank for a game that ships none, with its licence.
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${POP_ROOT}/third_party/soundfonts/generaluser-gs/GeneralUser-GS.sf2
            $<TARGET_BUNDLE_CONTENT_DIR:${target}>/general-midi.sf2
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${POP_ROOT}/third_party/soundfonts/generaluser-gs/LICENSE
            $<TARGET_BUNDLE_CONTENT_DIR:${target}>/general-midi-LICENSE.txt
    VERBATIM)
  if(EXISTS ${POP_BUILD_ROOT}/recomp/symbols.json)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${POP_BUILD_ROOT}/recomp/symbols.json
              $<TARGET_BUNDLE_CONTENT_DIR:${target}>/symbols.json
      VERBATIM)
  endif()
  # What a game's setup builds beside the app and its Mac package adds (NFSMW: tools/setup_kit/package.py):
  # the files it generated from the player's own game (build/generated: effect programs, light pools) into the
  # core mod's folder, and the first-run profile (registry, mod settings, NativeOptions.ini) into Defaults/,
  # which platform_ui_ios_prepare seeds a fresh profile from. Each only when the game has it.
  if(EXISTS ${POP_BUILD_ROOT}/generated AND EXISTS ${RECOMP_GAME_DIR}/mods/core/${RECOMP_GAME_ID})
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory ${POP_BUILD_ROOT}/generated
              $<TARGET_BUNDLE_CONTENT_DIR:${target}>/mods/core/${RECOMP_GAME_ID}
      COMMENT "Bundling the effects the setup generated (build/generated)"
      VERBATIM)
  endif()
  set(defaults_dir ${RECOMP_GAME_DIR}/tools/setup_kit/defaults)
  foreach(f ${defaults_dir}/registry.json ${defaults_dir}/mod-settings.json
            ${RECOMP_GAME_DIR}/mods/core/${RECOMP_GAME_ID}/NativeOptions.ini)
    if(EXISTS ${f})
      get_filename_component(name ${f} NAME)
      add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${f} $<TARGET_BUNDLE_CONTENT_DIR:${target}>/Defaults/${name}
        VERBATIM)
    endif()
  endforeach()
  # Last, so it overrides the signing set above: the sideloader (SideStore,
  # LiveContainer) signs the app and its embedded dylibs with its own identity.
  if(RECOMP_IOS_UNSIGNED)
    set_target_properties(${target} PROPERTIES
      XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED NO
      XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED NO
      XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY ""
      XCODE_ATTRIBUTE_DEVELOPMENT_TEAM ""
      XCODE_EMBED_FRAMEWORKS_CODE_SIGN_ON_COPY NO)
  endif()
endfunction()
