#version 450 core

#extension GL_ARB_shader_draw_parameters: enable

layout(binding = 0) uniform Block
{
    vec4 pos[2][4];
} block;

void main()
{
    if ((gl_BaseVertexARB > 0) || (gl_BaseInstanceARB > 0))
        gl_Position = block.pos[gl_VertexIndex % 2][gl_DrawIDARB % 4];
    else
        gl_Position = block.pos[gl_InstanceIndex % 2][gl_DrawIDARB % 4];
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST-DAG: call i32 @lgc.input.import.builtin.InstanceIndex{{.*}}
; SHADERTEST-DAG: call i32 @lgc.input.import.builtin.DrawIndex{{.*}}
; SHADERTEST-DAG: call i32 @lgc.input.import.builtin.VertexIndex{{.*}}
; SHADERTEST-DAG: call i32 @lgc.input.import.builtin.BaseInstance{{.*}}
; SHADERTEST-DAG: call i32 @lgc.input.import.builtin.BaseVertex{{.*}}
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
