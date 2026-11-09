idf_component_register(SRCS "padel-scoreboard.cpp" "score.cpp" "anim.cpp"
                    INCLUDE_DIRS ".")

# The animation code reproduces the reference JavaScript's floating point;
# keep multiply-adds unfused so the host check (test/) and the panel agree.
target_compile_options(${COMPONENT_LIB} PRIVATE -ffp-contract=off)
