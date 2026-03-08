cmake_minimum_required(VERSION 3.20)
project(QwenOCR LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)

add_executable(qwen_ocr WIN32 main.cpp)

target_compile_definitions(qwen_ocr PRIVATE UNICODE _UNICODE)

target_link_libraries(qwen_ocr PRIVATE
    user32 gdi32 gdiplus winhttp
    comctl32 comdlg32 shell32 ole32
)

# Output to exe folder
set_target_properties(qwen_ocr PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/../exe"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/../exe"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_SOURCE_DIR}/../exe"
)
