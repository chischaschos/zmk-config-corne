if(CONFIG_NICE_VIEW_KEY_DISPLAY_WIDGET)
  zephyr_library_sources(widgets/util.c)

  if(NOT CONFIG_ZMK_SPLIT OR CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    zephyr_library_sources(widgets/central_status.c)
  else()
    zephyr_library_sources(widgets/peripheral_status.c)
  endif()

  zephyr_library_sources(custom_status_screen.c)
endif()
