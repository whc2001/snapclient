
if(CONFIG_SNAPCLIENT_ENABLE_ETHERNET)

idf_component_register(SRCS "eth_interface.c"
                       INCLUDE_DIRS "include")

else()

idf_component_register()

endif()
