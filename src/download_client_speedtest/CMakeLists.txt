#
# Copyright (c) 2020 Nordic Semiconductor
#
# SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
#

include_directories(include)
zephyr_include_directories(include)
target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/download_client_speedtest.c)
target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/parse.c)
target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/http.c)
target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/sanity.c)                        
                        
                