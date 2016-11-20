#
# Install wb_presets
#
if(${VALIDATE_JSON})
  add_custom_target(
    validate_wbpresets_json ALL
    COMMAND ${jsonschema_BIN} -i ${CMAKE_CURRENT_SOURCE_DIR}/wb_presets.json ${CMAKE_CURRENT_SOURCE_DIR}/wb_presets.schema
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/check-wb_presets.py ${CMAKE_CURRENT_SOURCE_DIR}/wb_presets.json
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/wb_presets.json ${CMAKE_CURRENT_SOURCE_DIR}/wb_presets.schema ${CMAKE_CURRENT_SOURCE_DIR}/check-wb_presets.py
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/
    COMMENT "Checking validity of wb_presets.json"
  )
endif()
install(FILES wb_presets.json DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/darktable)
