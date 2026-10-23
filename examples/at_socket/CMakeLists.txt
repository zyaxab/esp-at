file(GLOB_RECURSE srcs *.c)

set(includes "include")

set(require_components at freertos lwip)

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS ${includes}
    REQUIRES ${require_components})

idf_component_set_property(${COMPONENT_NAME} WHOLE_ARCHIVE TRUE)
