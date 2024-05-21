
if(CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET OR CONFIG_SNAPCLIENT_USE_SPI_ETHERNET)

idf_component_register(SRCS "eth_interface.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES driver esp_eth)

else()

idf_component_register()

endif()
