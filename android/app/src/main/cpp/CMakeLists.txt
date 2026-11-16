cmake_minimum_required(VERSION 3.22.1)
project(pdfeditor_native CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The editing engine is shared with the desktop version.
set(CORE ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../src)
set(PDFIUM ${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party/pdfium)
set(JNILIBS ${CMAKE_CURRENT_SOURCE_DIR}/../jniLibs)

add_library(pdfium SHARED IMPORTED)
set_target_properties(pdfium PROPERTIES IMPORTED_LOCATION ${JNILIBS}/${ANDROID_ABI}/libpdfium.so)

add_library(pdfeditor SHARED
    engine.cpp
    imageio_android.cpp
    ${CORE}/pdf_doc.cpp
    ${CORE}/pdf_ops.cpp
    ${CORE}/sysfonts.cpp
)
target_include_directories(pdfeditor PRIVATE ${CORE} ${PDFIUM}/include ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(pdfeditor PRIVATE -Wno-unused-parameter -Wno-unused-function -fexceptions -frtti)
target_link_libraries(pdfeditor pdfium jnigraphics log android)
