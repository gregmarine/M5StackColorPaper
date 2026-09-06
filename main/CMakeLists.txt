idf_component_register(
    SRCS
        "main.cpp"
        "hal/hal.cpp"
        "hal/storage/hal_storage.cpp"
        "hal/utils/image/image_utils.cpp"
        "hal/utils/audio/audio.cpp"
        "apps/local_photo_slideshow/local_photo_slideshow.cpp"
        "apps/app_manager/app_manager.cpp"
        "net/wifi_ap.cpp"
        "net/ap_mode.cpp"
        "net/photo_api.cpp"
        "net/web_server.cpp"
    INCLUDE_DIRS
        "."
    REQUIRES
        nvs_flash
        fatfs
        wear_levelling
        esp_event
        esp_netif
        esp_wifi
        esp_http_server
        dns_server
        json
        M5Unified
        M5GFX
        tinyusb
    EMBED_FILES
        "${CMAKE_BINARY_DIR}/webapp/app.html.gz"
)

# The on-device web app is generated, not checked in: tools/build_webapp.py
# splices photo_lab.html's #core and #shared blocks into main/web/app.ui.html
# so the phone runs the same dither pipeline as the desktop tool. Generating it
# at build time is what makes that guarantee hold -- a committed copy would
# drift the moment photo_lab.html changed.
#
# Everything below has to come after idf_component_register(): the file is also
# read during IDF's early-expansion pass, where these commands do not belong.
idf_build_get_property(python PYTHON)

set(webapp_dir "${CMAKE_BINARY_DIR}/webapp")
set(webapp_html "${webapp_dir}/app.html")
set(webapp_gz "${webapp_dir}/app.html.gz")
set(webapp_ui "${CMAKE_CURRENT_SOURCE_DIR}/web/app.ui.html")
set(webapp_lab "${CMAKE_SOURCE_DIR}/tools/prepare_photo/photo_lab.html")
set(webapp_tool "${CMAKE_SOURCE_DIR}/tools/build_webapp.py")

add_custom_command(
    OUTPUT "${webapp_gz}" "${webapp_html}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${webapp_dir}"
    COMMAND "${python}" "${webapp_tool}"
            --ui "${webapp_ui}" --lab "${webapp_lab}" --out "${webapp_html}"
    DEPENDS "${webapp_ui}" "${webapp_lab}" "${webapp_tool}"
    COMMENT "Building on-device web app from photo_lab.html + app.ui.html"
    VERBATIM
)

# EMBED_FILES does not create a dependency edge to the command that generates
# the file, so without this the embed can run first and pick up a stale file,
# or nothing at all on a clean build.
add_custom_target(webapp_gz_target DEPENDS "${webapp_gz}")
add_dependencies(${COMPONENT_LIB} webapp_gz_target)
