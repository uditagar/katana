add_executable(lcc-cpu lcc.cpp)
add_dependencies(apps lcc-cpu)
target_link_libraries(lcc-cpu PRIVATE Katana::galois lonestar)
install(TARGETS lcc-cpu DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT apps EXCLUDE_FROM_ALL)

add_test_scale(small-ordered-relabel lcc-cpu INPUT rmat15_cleaned_symmetric INPUT_URI "${BASEINPUT}/propertygraphs/rmat15_cleaned_symmetric" NOT_QUICK NO_VERIFY -symmetricGraph -algo=orderedCount --relabel=true)
add_test_scale(small-ordered lcc-cpu  INPUT rmat15_cleaned_symmetric INPUT_URI "${BASEINPUT}/propertygraphs/rmat15_cleaned_symmetric" NOT_QUICK NO_VERIFY -symmetricGraph -algo=orderedCount)
add_test_scale(small-node lcc-cpu  INPUT rmat15_cleaned_symmetric INPUT_URI "${BASEINPUT}/propertygraphs/rmat15_cleaned_symmetric" NOT_QUICK NO_VERIFY  -symmetricGraph -algo=nodeiterator)
add_test_scale(small-edge lcc-cpu  INPUT rmat15_cleaned_symmetric INPUT_URI "${BASEINPUT}/propertygraphs/rmat15_cleaned_symmetric" NOT_QUICK NO_VERIFY -symmetricGraph -algo=edgeiterator)
