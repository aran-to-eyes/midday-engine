# DeterministicFP.cmake — the floating-point half of the determinism contract
# (spec section 15, constraint 5; plan m0-math-stdlib pins the full policy).
#
# Two mechanisms:
#   1. A configure-time guard: refuse any configuration that opts into
#      fast-math globally. scripts/check_fp_flags.py re-checks the emitted
#      compile_commands.json in CI, so both halves are falsifiable.
#   2. midday_apply_deterministic_fp(<target>): applied to every target that
#      participates in simulation. Bans fast-math and value-changing FMA
#      contraction drift (contraction is a per-compiler default; we pin it off
#      so the same source computes the same bits on every lane).

include_guard(GLOBAL)

function(_midday_fp_guard_flags var)
    if("${${var}}" MATCHES "-ffast-math|-Ofast|-funsafe-math-optimizations|-ffp-contract=fast|/fp:fast")
        message(FATAL_ERROR
            "DeterministicFP: ${var} opts into fast-math ('${${var}}'). "
            "Fast-math is banned repo-wide — determinism is contractual (spec section 15).")
    endif()
endfunction()

foreach(_midday_fp_var
        CMAKE_CXX_FLAGS
        CMAKE_CXX_FLAGS_DEBUG
        CMAKE_CXX_FLAGS_RELEASE
        CMAKE_CXX_FLAGS_RELWITHDEBINFO
        CMAKE_CXX_FLAGS_MINSIZEREL)
    _midday_fp_guard_flags(${_midday_fp_var})
endforeach()

function(midday_apply_deterministic_fp target)
    if(MSVC)
        # /fp:precise: no reassociation, no contraction into FMA by default.
        target_compile_options(${target} PRIVATE /fp:precise)
    else()
        # Clang, AppleClang, GCC: disable fast-math bundles and pin FMA
        # contraction off so optimization level cannot change results.
        target_compile_options(${target} PRIVATE -fno-fast-math -ffp-contract=off)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(i[3-6]86|x86)$")
            # 32-bit x87 excess precision would break bit-equality; the five
            # platform targets are all 64-bit, so this is a tripwire.
            message(FATAL_ERROR
                "DeterministicFP: 32-bit x86 (x87 FP) is not a supported target.")
        endif()
    endif()
endfunction()
