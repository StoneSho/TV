# LoveTV 纯净版：仅保留 LoveTV 界面 + 硬件驱动 + WiFi 配网
# 小智语音助手相关代码（application/protocols/ota/iot/audio_processing）已移除
set(SOURCES "audio_codecs/audio_codec.cc"
            "audio_codecs/no_audio_codec.cc"
            "display/display.cc"
            "display/no_display.cc"
            "lovetv/lovetv.cc"
            "lovetv/lovetv_config_server.cc"
            "lovetv/lovetv_mqtt.cc"
            "lovetv/lovetv_voice.cc"
            # 32px 中文标题子集（只含 芜湖起飞！），由 lv_font_conv 生成。
            # 放在 main/ 下而非 managed_components/，避免 idf.py reconfigure 覆盖
            "lovetv/font_lovetv_title_32.c"
            "system_info.cc"
            "settings.cc"
            "main.cc"
            )

set(INCLUDE_DIRS "." "display" "audio_codecs" "lovetv")

# 添加板级公共文件
file(GLOB BOARD_COMMON_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/boards/common/*.cc)
list(APPEND SOURCES ${BOARD_COMMON_SOURCES})
list(APPEND INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/boards/common)

# 纯 LoveTV 只支持星智 1.54TFT WiFi 这一块板子
set(BOARD_TYPE "xingzhi-cube-1.54tft-wifi")
file(GLOB BOARD_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/boards/${BOARD_TYPE}/*.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/boards/${BOARD_TYPE}/*.c
)
list(APPEND SOURCES ${BOARD_SOURCES})

# 根据Kconfig选择语言目录
if(CONFIG_LANGUAGE_ZH_CN)
    set(LANG_DIR "zh-CN")
elseif(CONFIG_LANGUAGE_EN_US)
    set(LANG_DIR "en-US")
endif()

# 定义生成路径
set(LANG_JSON "${CMAKE_CURRENT_SOURCE_DIR}/assets/${LANG_DIR}/language.json")
set(LANG_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/assets/lang_config.h")
file(GLOB ASSETS ${CMAKE_CURRENT_SOURCE_DIR}/assets/${LANG_DIR}/*.p3)

idf_component_register(SRCS ${SOURCES}
                    EMBED_FILES ${ASSETS}
                    INCLUDE_DIRS ${INCLUDE_DIRS}
                    WHOLE_ARCHIVE
                    )

# 使用 target_compile_definitions 来定义 BOARD_TYPE
target_compile_definitions(${COMPONENT_LIB}
                    PRIVATE BOARD_TYPE=\"${BOARD_TYPE}\"
                    )

# 添加生成规则
add_custom_command(
    OUTPUT ${LANG_HEADER}
    COMMAND python ${PROJECT_DIR}/scripts/gen_lang.py
            --input "${LANG_JSON}"
            --output "${LANG_HEADER}"
    DEPENDS
        ${LANG_JSON}
        ${PROJECT_DIR}/scripts/gen_lang.py
    COMMENT "Generating ${LANG_DIR} language config"
)

# 强制建立生成依赖
add_custom_target(lang_header ALL
    DEPENDS ${LANG_HEADER}
)
