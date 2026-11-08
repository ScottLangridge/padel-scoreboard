# IDF wrapper for the upstream DMD2 library (git submodule in ./DMD2, unmodified).
# DMD2's begin()/end() timer code has no ESP32 path, so the app calls
# beginNoTimer() and drives scanDisplay() itself.
idf_component_register(
    SRCS
        "DMD2/DMD2.cpp"
        "DMD2/DMDFrame.cpp"
        "DMD2/DMD2_Text.cpp"
        "DMD2/DMD2_TextBox.cpp"
        "DMD2/DMD2_Timer.cpp"
    INCLUDE_DIRS "DMD2"
    REQUIRES arduino-esp32
)

# Upstream code targets AVR; silence warnings it trips on with GCC 14 so they
# don't fail the build.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-function -Wno-error)

# The Arduino IDE implicitly includes Arduino.h before library sources; DMD2.h
# depends on that (byte, String, etc.).
target_compile_options(${COMPONENT_LIB} PRIVATE -include Arduino.h)
