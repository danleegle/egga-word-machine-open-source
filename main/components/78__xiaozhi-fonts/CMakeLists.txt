idf_component_register(
    SRCS
        "src/cbin_font.c"
        "src/DejaVuSans_Phonetic_16.c"
        "src/Zfull_GB_16.c"
        "src/Zfull_GB_20.c"
        "src/Zfull_GB_24.c"
        "src/CalibriLight_Eink_36.c"
        "src/font_egga_16_1.c"
        "src/font_egga_48_1.c"
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        "lvgl__lvgl"
)

# 为组件添加编译宏定义
target_compile_definitions(${COMPONENT_LIB} PUBLIC LV_LVGL_H_INCLUDE_SIMPLE)
