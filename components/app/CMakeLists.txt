idf_component_register(
    SRCS "sea_model.c" "app.c" "app_screen.c" "app_upload.c" "app_serve.c" "app_sea.c"
    INCLUDE_DIRS "include"
    REQUIRES ui theme haptic net
)
