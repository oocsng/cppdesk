# =============================================================================
# AeroDesk x86-64 AVX2 SIMD Assembly Kernels (Windows x64 Calling Convention)
# Volatile GPRs: rax, rcx, rdx, r8, r9, r10, r11
# Volatile YMMs: ymm0 - ymm5
# =============================================================================

    .text
    .globl aerodesk_avx2_tile_diff
    .globl aerodesk_avx2_hash_tile
    .globl aerodesk_avx2_xor_blocks32
    .globl cppdesk_avx2_tile_diff
    .globl cppdesk_avx2_hash_tile
    .globl cppdesk_avx2_xor_blocks32

    .set cppdesk_avx2_tile_diff, aerodesk_avx2_tile_diff
    .set cppdesk_avx2_hash_tile, aerodesk_avx2_hash_tile
    .set cppdesk_avx2_xor_blocks32, aerodesk_avx2_xor_blocks32

# -----------------------------------------------------------------------------
# int aerodesk_avx2_tile_diff(
#     const uint8_t* prevTilePtr, // rcx
#     const uint8_t* currTilePtr, // rdx
#     int strideBytes,            // r8d
#     int widthBytes,             // r9d
#     int heightRows              // 40(%rsp)
# )
# Returns 1 in eax if any byte differs, 0 if identical.
# -----------------------------------------------------------------------------
    .p2align 4
aerodesk_avx2_tile_diff:
    movl    40(%rsp), %r10d         # r10d = heightRows
    testl   %r10d, %r10d
    jle     .Ldiff_equal
    testl   %r9d, %r9d
    jle     .Ldiff_equal

    movslq  %r8d, %r8               # r8 = strideBytes (64-bit)
    movslq  %r9d, %r9               # r9 = widthBytes (64-bit)

.Ldiff_row_loop:
    xorq    %r11, %r11              # r11 = col byte offset = 0
    leaq    -32(%r9), %rax          # rax = widthBytes - 32
    cmpq    $32, %r9
    jl      .Ldiff_tail_check

.Ldiff_vec32_loop:
    vmovdqu (%rcx, %r11), %ymm0
    vmovdqu (%rdx, %r11), %ymm1
    vpcmpeqb %ymm1, %ymm0, %ymm2
    vpmovmskb %ymm2, %eax
    cmpl    $0xFFFFFFFF, %eax
    jne     .Ldiff_found

    addq    $32, %r11
    leaq    32(%r11), %rax
    cmpq    %r9, %rax
    jle     .Ldiff_vec32_loop

.Ldiff_tail_check:
    cmpq    %r9, %r11
    jge     .Ldiff_next_row

.Ldiff_tail_loop:
    movzbl  (%rcx, %r11), %eax
    cmpb    (%rdx, %r11), %al
    jne     .Ldiff_found
    incq    %r11
    cmpq    %r9, %r11
    jl      .Ldiff_tail_loop

.Ldiff_next_row:
    addq    %r8, %rcx
    addq    %r8, %rdx
    decl    %r10d
    jnz     .Ldiff_row_loop

.Ldiff_equal:
    vzeroupper
    xorl    %eax, %eax
    ret

.Ldiff_found:
    vzeroupper
    movl    $1, %eax
    ret

# -----------------------------------------------------------------------------
# uint64_t aerodesk_avx2_hash_tile(
#     const uint8_t* tilePtr,     // rcx
#     int strideBytes,            // edx
#     int widthBytes,             // r8d
#     int heightRows              // r9d
# )
# Returns 64-bit SIMD hash in rax.
# -----------------------------------------------------------------------------
    .p2align 4
aerodesk_avx2_hash_tile:
    movabsq $0xCBF29CE484222325, %rax   # FNV-1a 64-bit offset basis
    testl   %r8d, %r8d
    jle     .Lhash_done_scalar
    testl   %r9d, %r9d
    jle     .Lhash_done_scalar

    movslq  %edx, %rdx                  # rdx = strideBytes (64-bit)
    movslq  %r8d, %r8                   # r8  = widthBytes (64-bit)

    # Broadcast initial seed into ymm0 and alpha mask (0xFF000000FF000000) into ymm3
    vmovq   %rax, %xmm0
    vpbroadcastq %xmm0, %ymm0
    movabsq $0xFF000000FF000000, %r10
    vmovq   %r10, %xmm3
    vpbroadcastq %xmm3, %ymm3

    movabsq $0x100000001B3, %r10        # FNV-1a 64-bit prime

.Lhash_row_loop:
    xorq    %r11, %r11                  # r11 = byte offset in row
    cmpq    $32, %r8
    jl      .Lhash_tail_check

.Lhash_vec32_loop:
    vmovdqu (%rcx, %r11), %ymm1
    vpor    %ymm3, %ymm1, %ymm1         # Normalize alpha channel to 0xFF
    vmovq   %r11, %xmm4
    vpbroadcastq %xmm4, %ymm4
    vpaddq  %ymm4, %ymm1, %ymm1         # Mix column byte offset so swapped blocks differ
    vpxor   %ymm1, %ymm0, %ymm0
    vpsllq  $13, %ymm0, %ymm2
    vpsrlq  $51, %ymm0, %ymm4
    vpor    %ymm4, %ymm2, %ymm0         # Rotate-left 13 bits per 64-bit lane
    vpaddq  %ymm1, %ymm0, %ymm0

    addq    $32, %r11
    leaq    32(%r11), %rax
    cmpq    %r8, %rax
    jle     .Lhash_vec32_loop

.Lhash_tail_check:
    # Fold ymm0 after row or process scalar tail
    cmpq    %r8, %r11
    jge     .Lhash_next_row

.Lhash_tail_loop:
    movzbl  (%rcx, %r11), %eax
    testb   $1, %r11b
    jz      .Lhash_tail_mix
    testb   $2, %r11b
    jz      .Lhash_tail_mix
    movl    $0xFF, %eax                 # Normalize alpha channel (offset % 4 == 3) to 0xFF
.Lhash_tail_mix:
    addq    %r11, %rax                  # Mix byte position so swapped/reordered tail bytes differ
    imulq   %r10, %rax                  # Spread bits with FNV-1a prime
    vmovq   %rax, %xmm1
    vpxor   %ymm1, %ymm0, %ymm0
    vpsllq  $13, %ymm0, %ymm2
    vpsrlq  $51, %ymm0, %ymm4
    vpor    %ymm4, %ymm2, %ymm0         # Rotate-left 13 bits per 64-bit lane
    vpaddq  %ymm1, %ymm0, %ymm0
    incq    %r11
    cmpq    %r8, %r11
    jl      .Lhash_tail_loop

.Lhash_next_row:
    # Mix row index so vertically shifted rows produce distinct hashes
    vmovd   %r9d, %xmm2
    vpbroadcastq %xmm2, %ymm2
    vpaddq  %ymm2, %ymm0, %ymm0
    vpsllq  $7, %ymm0, %ymm1
    vpxor   %ymm1, %ymm0, %ymm0

    addq    %rdx, %rcx
    decl    %r9d
    jnz     .Lhash_row_loop

    # Non-cancelling horizontal reduction of 256-bit ymm0 -> 64-bit rax
    vextracti128 $1, %ymm0, %xmm1
    vpaddq  %xmm1, %xmm0, %xmm0
    vmovq   %xmm0, %rax
    vpextrq $1, %xmm0, %r11
    imulq   %r10, %rax
    addq    %r11, %rax
    imulq   %r10, %rax
    orq     $1, %rax                    # Ensure valid tile hash is never 0 (0 = uninitialized sentinel)

    vzeroupper
    ret

.Lhash_done_scalar:
    ret

# -----------------------------------------------------------------------------
# void aerodesk_avx2_xor_blocks32(
#     uint8_t* data,              // rcx
#     const uint8_t* ksBuffer,    // rdx
#     size_t blocks32             // r8
# )
# XORs `blocks32 * 32` bytes in-place using 256-bit YMM registers.
# -----------------------------------------------------------------------------
    .p2align 4
aerodesk_avx2_xor_blocks32:
    testq   %r8, %r8
    jz      .Lxor_done

.Lxor_loop:
    vmovdqu (%rcx), %ymm0
    vmovdqu (%rdx), %ymm1
    vpxor   %ymm1, %ymm0, %ymm0
    vmovdqu %ymm0, (%rcx)
    addq    $32, %rcx
    addq    $32, %rdx
    decq    %r8
    jnz     .Lxor_loop

    vzeroupper
.Lxor_done:
    ret
