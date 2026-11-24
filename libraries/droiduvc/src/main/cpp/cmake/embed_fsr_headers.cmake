if(NOT DEFINED FSR_A OR NOT DEFINED FSR_FSR1 OR NOT DEFINED OUTPUT_HEADER)
    message(FATAL_ERROR "FSR_A, FSR_FSR1 and OUTPUT_HEADER are required")
endif()

file(READ "${FSR_A}" FSR_A_CONTENT)
file(READ "${FSR_FSR1}" FSR_FSR1_CONTENT)

file(WRITE "${OUTPUT_HEADER}"
    "#pragma once\n"
    "namespace droiduvc {\n"
    "static const char kFfxPortabilityHeader[] = R\"ITXFSRA(\n"
    "${FSR_A_CONTENT}\n"
    ")ITXFSRA\";\n"
    "static const char kFfxFsr1Header[] = R\"ITXFSR1(\n"
    "${FSR_FSR1_CONTENT}\n"
    ")ITXFSR1\";\n"
    "} // namespace droiduvc\n"
)
