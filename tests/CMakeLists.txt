# Тесты и стендовые утилиты. Включаются опцией: cmake -DBUILD_TESTS=ON ..

# ConfigLoader: без железа и без MIDAS, запускается через ctest
add_executable(test_config_loader test_config_loader.cpp)
target_link_libraries(test_config_loader danss_core)
add_test(NAME config_loader COMMAND test_config_loader ${PROJECT_SOURCE_DIR})

# OwonPSU / PowerSupplyController против mock_owon_server.py (или реального прибора — осторожно, 55 В)
add_executable(test_owon_psu test_owon_psu.cpp)
target_link_libraries(test_owon_psu danss_core)

# PsuWorker (поток управления блоком питания) против mock_owon_server.py
add_executable(test_psu_worker test_psu_worker.cpp)
target_link_libraries(test_psu_worker danss_core)

# Пошаговая проверка блока питания на стенде (info | limits | cpu-on | cpu-off | hv | interlock)
add_executable(psu_bench psu_bench.cpp)
target_link_libraries(psu_bench danss_core)
