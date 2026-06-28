# Work around espressif/usb_device_uac exporting its descriptor source as a
# PUBLIC TinyUSB source. PlatformIO/SCons then sees usb_descriptors.c twice:
# once under TinyUSB and once under usb_device_uac with different compile flags.
idf_build_get_property(build_components BUILD_COMPONENTS)
set(tinyusb_component "")
if(espressif__tinyusb IN_LIST build_components)
    set(tinyusb_component espressif__tinyusb)
elseif(tinyusb IN_LIST build_components)
    set(tinyusb_component tinyusb)
endif()

if(tinyusb_component)
    idf_component_get_property(tusb_lib ${tinyusb_component} COMPONENT_LIB)
    foreach(source_property SOURCES INTERFACE_SOURCES)
        get_target_property(tusb_sources ${tusb_lib} ${source_property})
        if(tusb_sources)
            set(filtered_tusb_sources "")
            foreach(src IN LISTS tusb_sources)
                if(NOT src MATCHES "usb_device_uac[/\\\\]tusb[/\\\\]usb_descriptors\\.c$")
                    list(APPEND filtered_tusb_sources "${src}")
                endif()
            endforeach()
            set_target_properties(${tusb_lib} PROPERTIES
                ${source_property} "${filtered_tusb_sources}")
        endif()
    endforeach()
endif()
