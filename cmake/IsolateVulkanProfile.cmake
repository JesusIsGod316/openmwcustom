# Keep bare-executable launches isolated too. Used only for Windows Vulkan builds.
function(isolate_vulkan_profile config_file)
    file(READ "${config_file}" content)
    foreach(pair IN ITEMS "data-local|?userdata?data|?local?UserProfile/local-data"
                          "user-data|?userdata?|?local?UserProfile/user-data"
                          "config|?userconfig?|?local?UserProfile")
        string(REPLACE "|" ";" fields "${pair}")
        list(GET fields 0 key)
        list(GET fields 1 old)
        list(GET fields 2 new)
        set(needle "${key}=\"${old}\"")
        string(FIND "${content}" "${needle}" found)
        if(found EQUAL -1)
            message(FATAL_ERROR "Cannot isolate ${config_file}: expected ${needle}")
        endif()
        string(REPLACE "${needle}" "${key}=\"${new}\"" content "${content}")
    endforeach()
    file(WRITE "${config_file}" "${content}")
endfunction()
