IF ( NOT (CONFIG_IDF_TARGET MATCHES "esp32s2"))
set(COMPONENT_ADD_INCLUDEDIRS driver/i2c_bus)
ELSEIF (CONFIG_IDF_TARGET MATCHES "esp32s2")
set(COMPONENT_ADD_INCLUDEDIRS driver/i2c_bus)
ENDIF ( NOT (CONFIG_IDF_TARGET MATCHES "esp32s2"))


# Edit following two lines to set component requirements (see docs)
set(COMPONENT_REQUIRES driver audio_hal audio_sal audio_board )
set(COMPONENT_PRIV_REQUIRES )
IF ( NOT (CONFIG_IDF_TARGET MATCHES "esp32s2"))
set(COMPONENT_SRCS driver/i2c_bus/i2c_bus.c)

ELSEIF (CONFIG_IDF_TARGET MATCHES "esp32s2")
set(COMPONENT_SRCS driver/i2c_bus/i2c_bus.c )
ENDIF ( NOT (CONFIG_IDF_TARGET MATCHES "esp32s2"))

register_component()

set_property(TARGET ${COMPONENT_LIB} APPEND PROPERTY LINK_INTERFACE_MULTIPLICITY 3)
