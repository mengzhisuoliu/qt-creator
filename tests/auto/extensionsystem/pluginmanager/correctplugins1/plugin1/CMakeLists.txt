add_qtc_plugin(correct_plugin1
  PLUGIN_NAME plugin1
  DEPENDS ExtensionSystem Utils
  SOURCES
    plugin1.cpp plugin1.h
  PLUGIN_PATH "${TEST_PLUGIN_PATH}"
  SKIP_INSTALL
  INTERNAL_ONLY
)
