cmake_minimum_required(VERSION 3.20)
project(dx12native CXX)
set(CMAKE_CXX_STANDARD 20)

find_package(JNI REQUIRED)

# --- DLL que carga el mod por JNI ---
add_library(dx12native SHARED src/dx12_probe.cpp src/dx12_core.cpp)
target_include_directories(dx12native PRIVATE ${JNI_INCLUDE_DIRS})
target_link_libraries(dx12native PRIVATE d3d12 dxgi d3dcompiler)

# Copia la DLL a los recursos del mod
add_custom_command(TARGET dx12native POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:dx12native>
		${CMAKE_SOURCE_DIR}/../src/client/resources/natives/windows-x64/dx12native.dll)

# --- Paso 2: triangulo de prueba independiente ---
add_executable(dx12_triangle src/triangle_test.cpp)
target_link_libraries(dx12_triangle PRIVATE d3d12 dxgi d3dcompiler)
