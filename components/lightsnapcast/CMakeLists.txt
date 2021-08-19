idf_component_register(SRCS "snapcast.c" "player.c"
                       INCLUDE_DIRS "include"
                       REQUIRES custom_driver custom_board libbuffer json libmedian audio_board)
