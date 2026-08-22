# The default build runs the pure conversion-logic tests, which need no
# hardware. Setting CONFIG_TG28_SW_TEST_WITH_HARDWARE (Kconfig.projbuild)
# swaps in the on-hardware tests, which talk to a real TG28 over I2C.
if(CONFIG_TG28_SW_TEST_WITH_HARDWARE)
    set(TEST_APP_SOURCES "test_tg28_sw_hw.c")
else()
    set(TEST_APP_SOURCES "test_tg28_sw.c")
endif()

idf_component_register(SRCS "${TEST_APP_SOURCES}"
                       INCLUDE_DIRS ".")

# The test app needs tg28_sw_priv.h, which lives in the component's private
# include directory. Attach it to the target directly instead of listing it in
# INCLUDE_DIRS: the component validation check warns about include directories
# that belong to another component (components/tg28_sw/priv_include), and this
# keeps the header private to the test app.
target_include_directories(${COMPONENT_LIB} PRIVATE "${COMPONENT_DIR}/../../priv_include")
