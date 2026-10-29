.set noreorder
// mips ISA has explicit delay slots
// (i.e. instruction after branches/jumps are always unconditionally executed)
#include "AsmCommon.i"

// NOTE: vclipw.xyz takes 4 cycles to produce result, which must be accounted for

// Note that registers are numbered for N32 ABI, but when disassembling
//   in ghidra or PCSX2, they are disassembled as O32 ABI ?
// https://github.com/ps2dev/binutils-gdb/blob/e9cf3691bfa140469d52815a2307b00eecf7917c/gas/config/tc-mips.c#L2786

# transform temp registers
	#define POSCL $vf10 // TRANSFORMED(POS_[1234]) * CLIP_PLANES_ADJUST
	#define POS_1 $vf11 // vertex 1 position
	#define POS_2 $vf12 // vertex 2 position
	#define POS_3 $vf13 // vertex 3 position
	#define POS_4 $vf14 // vertex 4 position
	#define FOGS  $vf15 // fog values of vertices (1 in x, 2 in y, 3 in z, 4 in w)
	#define CRS1  $vf16 // temp for face culling
	#define CRS2  $vf17 // temp for face culling

	#define _one   $vf0w
	#define POS1w $vf11w
	#define POS2w $vf12w
	#define POS3w $vf13w
	#define POS4w $vf14w

	#define SRC $a0
	#define DST $a1
	#define TMP $a2

	#define COL1 $f12
	#define COL2 $f13
	#define COL3 $f14
	#define COL4 $f15

	#define Z_1 $f0
	#define Z_2 $f1
	#define Z_3 $f2
	#define Z_4 $f3

	#define W_1 $f16
	#define W_2 $f17
	#define W_3 $f18
	#define W_4 $f19

	#define XY_1 $t1
	#define XY_2 $t2
	#define XY_3 $t3
	#define XY_4 $a3

	#define Y_1 $a4
	#define Y_2 $a5
	#define Y_3 $a6
	#define Y_4 $a7


.macro TransformVertex vpos
	vmulaw		$ACC,  MVP4, V0001 # ACC[xyzw] = mvp.row3[xyzw] * 1.0f; (vf0.w is 1)
	vmaddax		$ACC,  MVP1, \vpos # ACC[xyzw] = ACC[xyzw] + mvp.row0[xyzw] * VEC.x
	vmadday		$ACC,  MVP2, \vpos # ACC[xyzw] = ACC[xyzw] + mvp.row1[xyzw] * VEC.y
	vmaddz		\vpos, MVP3, \vpos # VEC[xyzw] = ACC[xyzw] + mvp.row2[xyzw] * VEC.z
.endm

.macro BeginClip vpos
	vmul		POSCL, \vpos, CL_F  # TMP = TRANSFORMED(VEC) * CLIP_PLANES_ADJUST
	# begin clip flags calculation
	vclipw.xyz	POSCL, POSCL    	# CLIP_FLAGS.append(CLIP(TMP.xyz, TMP.w))
.endm

.macro VPTransform vpos
	vmulw.xyz	\vpos, \vpos, \vpos # TMP.xyz = IN.xyz * IN.w (inverse W)
	vmul.xyz	\vpos, \vpos, VP_S  # TMP.xyz = TMP * viewport_scale
	vadd.xyz	\vpos, \vpos, VP_O  # TMP.xyz = TMP + viewport_origin
.endm

// Calculates fog value for a vertex from its clip space W
//	(must be done before W is replaced by 1/W)
.macro CalcFog lane, vpos
	vmulw.\lane	FOGS, FOG_S, \vpos # FOGS.lane = fog_scale * VEC.w
.endm

// Combines Z (24 bits) and F (8 bits) of a vertex for XYZF2 register
//	$t0, $t8 = temp registers
.macro PackZF z_offset, f_offset
	lw			$t0, \z_offset(TMP)
	lw			$t8, \f_offset(TMP)
	sll			$t8, $t8, 24
	or			$t0, $t0, $t8
	sw			$t0, \z_offset(TMP)
.endm

// Fully transforms 4 vertices with size of 16 bytes
//	$a0 = addresss of src  vertices
//	$a1 = addresss of dst  vertices
//   $a2 = address of  tmp  vertex
//	$v0 = address of final vertices (return value)
BEG_FUNC(DrawColouredQuad)

### VERTEX 1 ###
	lqc2		POS_1, 0x00(SRC)

	TransformVertex POS_1
	CalcFog x, POS_1
	lwc1		COL1, 0x0C(SRC)
	vdiv	 	$Q,   _one, POS1w
	BeginClip POS_1

### VERTEX 2 ###
	lqc2		POS_2, 0x10(SRC)

	TransformVertex POS_2
	CalcFog y, POS_2
	lwc1		COL2,  0x1C(SRC)
	vmulq.w 	POS_1, V0001, $Q
	vdiv	 	$Q,    _one, POS2w
	BeginClip POS_2

### VERTEX 3 ###
	lqc2		POS_3, 0x20(SRC)

	TransformVertex POS_3
	CalcFog z, POS_3
	lwc1		COL3,  0x2C(SRC)
	vmulq.w 	POS_2, V0001, $Q
	vdiv 		$Q,    _one, POS3w
	BeginClip POS_3

### VERTEX 4 ###
	lqc2		POS_4, 0x30(SRC)

	TransformVertex POS_4
	CalcFog w, POS_4
	lwc1		COL4,  0x3C(SRC)
	vmulq.w 	POS_3, V0001, $Q
	vdiv 		$Q,    _one, POS4w
	BeginClip POS_4

	vnop # adjust for delay
	vnop # adjust for delay
	vnop # adjust for delay

	# STORE CLIP FLAGS 4 RESULT
	vwaitq
	vmulq.w 	POS_4, V0001, $Q

	# check if any vertices would need clipping
	cfc2 		$t0, $18 # t0 = VP0_REGS[CLIP_FLAGS]
	bnez 		$t0, any_clipped_vertices
	nop

	# output vertices
	VPTransform POS_1
	VPTransform POS_2
	VPTransform POS_3
	VPTransform POS_4

	# calculate screen space winding (for face culling)
	# CRS1.z = (P2 - P1).x * (P3 - P1).y - (P2 - P1).y * (P3 - P1).x
	vsub.xy		CRS1, POS_2, POS_1
	vsub.xy		CRS2, POS_3, POS_1
	vopmula.xyz	$ACC, CRS1, CRS2
	vopmsub.xyz	CRS1, CRS2, CRS1

	# finish fog calculation: F = clamp(fog_scale * W + fog_bias, 0, 255)
	vadd.xyzw	FOGS, FOGS, FOG_B
	vmaxx.xyzw	FOGS, FOGS, V0001 # V0001.x is 0
	vminix.xyzw	FOGS, FOGS, FOG_M
	vftoi0.xyzw	FOGS, FOGS

	vftoi0.xyz	POS_1, POS_1
	vftoi0.xyz	POS_2, POS_2
	vftoi0.xyz	POS_3, POS_3
	vftoi0.xyz	POS_4, POS_4

	# Convert to register format
	sqc2		POS_1, 0x00(TMP)
	sqc2		POS_2, 0x10(TMP)
	sqc2		POS_3, 0x20(TMP)
	sqc2		POS_4, 0x30(TMP)
	sqc2		FOGS,  0x40(TMP)
	sqc2		CRS1,  0x50(TMP)

	# check if quad should be culled
	# (cull mask 0 = no culling, otherwise sign of mask = winding to cull)
	lui			$t8, %hi(gfx_cullMask)
	lw			$t8, %lo(gfx_cullMask)($t8)
	lw			$t0, 0x58(TMP)
	beqz		$t8, not_culled
	xor			$t0, $t0, $t8
	bgez		$t0, quad_culled
	nop
not_culled:

	PackZF 0x08, 0x40
	PackZF 0x18, 0x44
	PackZF 0x28, 0x48
	PackZF 0x38, 0x4C

	lhu			XY_1, 0x00(TMP)
	lhu			 Y_1, 0x04(TMP)
	lwc1		 Z_1, 0x08(TMP)
	lwc1		 W_1, 0x0C(TMP)

	lhu			XY_2, 0x10(TMP)
	lhu			 Y_2, 0x14(TMP)
	lwc1		 Z_2, 0x18(TMP)
	lwc1		 W_2, 0x1C(TMP)

	lhu			XY_3, 0x20(TMP)
	lhu			 Y_3, 0x24(TMP)
	lwc1		 Z_3, 0x28(TMP)
	lwc1		 W_3, 0x2C(TMP)

	lhu			XY_4, 0x30(TMP)
	lhu			 Y_4, 0x34(TMP)
	lwc1		 Z_4, 0x38(TMP)
	lwc1		 W_4, 0x3C(TMP)

	sll 		Y_1, Y_1, 16
	sll			Y_2, Y_2, 16
	sll 		Y_3, Y_3, 16
	sll 		Y_4, Y_4, 16

	or			XY_1, XY_1, Y_1
	or			XY_2, XY_2, Y_2
	or			XY_3, XY_3, Y_3
	or			XY_4, XY_4, Y_4

	# write 1,2,3 3,4,1
	swc1 COL1, (0*S_PCOL + PCOL__C)(DST)
	swc1  W_1, (0*S_PCOL + PCOL__W)(DST)
	sw   XY_1, (0*S_PCOL + PCOL_XY)(DST)
	swc1  Z_1, (0*S_PCOL + PCOL__Z)(DST)

	swc1 COL2, (1*S_PCOL + PCOL__C)(DST)
	swc1  W_2, (1*S_PCOL + PCOL__W)(DST)
	sw   XY_2, (1*S_PCOL + PCOL_XY)(DST)
	swc1  Z_2, (1*S_PCOL + PCOL__Z)(DST)

	swc1 COL3, (2*S_PCOL + PCOL__C)(DST)
	swc1  W_3, (2*S_PCOL + PCOL__W)(DST)
	sw   XY_3, (2*S_PCOL + PCOL_XY)(DST)
	swc1  Z_3, (2*S_PCOL + PCOL__Z)(DST)

	swc1 COL3, (3*S_PCOL + PCOL__C)(DST)
	swc1  W_3, (3*S_PCOL + PCOL__W)(DST)
	sw   XY_3, (3*S_PCOL + PCOL_XY)(DST)
	swc1  Z_3, (3*S_PCOL + PCOL__Z)(DST)

	swc1 COL4, (4*S_PCOL + PCOL__C)(DST)
	swc1  W_4, (4*S_PCOL + PCOL__W)(DST)
	sw   XY_4, (4*S_PCOL + PCOL_XY)(DST)
	swc1  Z_4, (4*S_PCOL + PCOL__Z)(DST)

	swc1 COL1, (5*S_PCOL + PCOL__C)(DST)
	swc1  W_1, (5*S_PCOL + PCOL__W)(DST)
	sw   XY_1, (5*S_PCOL + PCOL_XY)(DST)
	swc1  Z_1, (5*S_PCOL + PCOL__Z)(DST)

	addi DST, 6 * S_PCOL

quad_culled:
	jr $ra
	move $v0, DST

any_clipped_vertices:
	# CLIP_FLAGS holds 6 flags for each of the 4 vertices. If all 4 vertices are
	#  outside the same plane, then the quad can't be visible, so just discard it
	#  (e.g. quads behind the camera, which would otherwise be clipped in C)
	srl			$t1, $t0, 6
	and			$t1, $t1, $t0
	srl			$t2, $t0, 12
	and			$t1, $t1, $t2
	srl			$t2, $t0, 18
	and			$t1, $t1, $t2
	andi		$t1, $t1, 0x3F
	bnez		$t1, quad_culled
	nop

	# NULL is returned so that the caller clips the quad instead
	jr $ra
	move $v0, $zero
END_FUNC(DrawColouredQuad)
