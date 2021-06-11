project(sockdemo)

add_executable(select_demo ioselect.cpp)
add_executable(asynselect asynselect.cpp)
target_link_libraries(select_demo Ws2_32.lib)
target_link_libraries(asynselect Ws2_32.lib)