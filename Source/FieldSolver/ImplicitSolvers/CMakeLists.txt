foreach(D IN LISTS WarpX_DIMS)
    warpx_set_suffix_dims(SD ${D})
    target_sources(lib_${SD}
      PRIVATE
        SemiImplicitEM.cpp
        ThetaImplicitEM.cpp
        WarpXImplicitOps.cpp
        WarpXSolverVec.cpp
    )
endforeach()
