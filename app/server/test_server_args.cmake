foreach(value IN ITEMS "-1" "65536" "70000" "1x" "1.5" "" "99999999999999999999999999")
    execute_process(COMMAND "${SERVER}" 18999 --media-port "${value}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 5)
    if(NOT result STREQUAL "1" OR NOT error MATCHES "Invalid media port:")
        message(FATAL_ERROR "Invalid media port '${value}' was not rejected: ${result} ${error}")
    endif()
endforeach()
foreach(option IN ITEMS "--media-port" "--listen-ip" "--announced-ip" "--unknown"
        "--mode" "--p2p-max" "--downgrade-delay-ms" "--web-root" "--port" "--bind")
    execute_process(COMMAND "${SERVER}" 18999 "${option}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 5)
    if(NOT result STREQUAL "1" OR NOT error MATCHES "Unknown or incomplete option:")
        message(FATAL_ERROR "Incomplete option '${option}' was not rejected: ${result} ${error}")
    endif()
endforeach()
foreach(value IN ITEMS "0" "1" "9" "-1" "2x" "2.5" "99999999999999999999999999")
    execute_process(COMMAND "${SERVER}" 18999 --p2p-max "${value}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 5)
    if(NOT result STREQUAL "1" OR NOT error MATCHES "Invalid P2P maximum:")
        message(FATAL_ERROR "Invalid P2P maximum '${value}' was not rejected: ${result} ${error}")
    endif()
endforeach()
foreach(value IN ITEMS "-1" "1x" "1.5" "4294967296")
    execute_process(COMMAND "${SERVER}" 18999 --downgrade-delay-ms "${value}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 5)
    if(NOT result STREQUAL "1" OR NOT error MATCHES "Invalid downgrade delay:")
        message(FATAL_ERROR "Invalid downgrade delay '${value}' was not rejected: ${result} ${error}")
    endif()
endforeach()
foreach(arguments IN ITEMS "--mode;unknown" "--mode;sfu;--mesh" "--mode;mesh;--mode;auto"
        "--web-root;this-directory-does-not-exist" "--bind;not-an-ip"
        "--announced-ip;not-an-ip")
    execute_process(COMMAND "${SERVER}" 18999 ${arguments}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 5)
    if(NOT result STREQUAL "1")
        message(FATAL_ERROR "Invalid configuration '${arguments}' was not rejected: ${result} ${error}")
    endif()
endforeach()
foreach(value IN ITEMS "0" "65536" "abc")
    execute_process(COMMAND "${SERVER}" "${value}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 5)
    if(NOT result STREQUAL "1" OR NOT error MATCHES "Invalid signaling port:")
        message(FATAL_ERROR "Invalid signaling port '${value}' was not rejected: ${result} ${error}")
    endif()
endforeach()
