#!/usr/bin/env python3
"""Build production NVN pipeline/clip code against host stubs and validate SPIR-V.
Requires clang++, spirv-as and spirv-val. No Vulkan device or game execution.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'private/build-linux'


def run(*args, **kwargs):
    subprocess.run(args, check=True, **kwargs)


def main():
    target = 'CMakeFiles/bayonetta3.dir/runtime/src/nvn/nvn_pipeline.cpp.o'
    command = subprocess.check_output(['ninja', '-C', str(BUILD), '-t', 'commands', target], text=True).splitlines()[-1]
    words = shlex.split(command)
    flags = []
    for word in words[1:]:
        if word.startswith(('-I', '-D', '-std=', '-march=')):
            flags.append(word)
    with tempfile.TemporaryDirectory(prefix='b3-nvn-pipeline-') as temp:
        tmp = Path(temp)
        exe = tmp / 'test'
        run(words[0], *flags, '-O0', '-ffunction-sections', '-fdata-sections',
            str(ROOT / 'tests/bayonetta3_nvn_pipeline_test.cpp'),
            str(ROOT / 'runtime/src/nvn/nvn_spirv_clip.cpp'),
            str(ROOT / 'runtime/src/nvn/nvn_spirv_alpha.cpp'),
            '-Wl,--gc-sections', '-pthread', '-o', str(exe))
        # Test both supported Position declarations, including a nonzero block
        # member index (guards against accidentally patching another output).
        for block in [False, True]:
            asm = tmp / 'fixture.spvasm'
            declaration = '''OpMemberDecorate %block 1 BuiltIn Position
OpMemberDecorate %block 0 BuiltIn PointSize
OpDecorate %block Block''' if block else 'OpDecorate %position BuiltIn Position'
            types = '''%block = OpTypeStruct %float %vec4
%block_ptr = OpTypePointer Output %block
%position = OpVariable %block_ptr Output
%uint = OpTypeInt 32 0
%one = OpConstant %uint 1''' if block else '%position = OpVariable %ptr Output'
            store = '''%p = OpAccessChain %ptr %position %one
OpStore %p %value''' if block else 'OpStore %position %value'
            asm.write_text(f'''OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint Vertex %main "main" %position
{declaration}
%void = OpTypeVoid
%fn = OpTypeFunction %void
%float = OpTypeFloat 32
%vec4 = OpTypeVector %float 4
%ptr = OpTypePointer Output %vec4
{types}
%zero = OpConstant %float 0
%value = OpConstantComposite %vec4 %zero %zero %zero %zero
%main = OpFunction %void None %fn
%label = OpLabel
{store}
OpReturn
OpFunctionEnd
''')
            spv = tmp / 'fixture.spv'
            out = tmp / 'patched.spv'
            run('spirv-as', '--target-env', 'vulkan1.1', str(asm), '-o', str(spv))
            run('spirv-val', '--target-env', 'vulkan1.1', str(spv))
            for extension in [0, 1]:
                for forced in [0, 1]:
                    env = dict(os.environ)
                    for name in ['NX_SPIRV_OPT', 'NX_SPIRV_TOOL']:
                        env.pop(name, None)
                    env['NX_CLIP_Z_PATCH'] = str(forced)
                    run(str(exe), str(spv), str(out), str(extension), str(forced), env=env)
                    run('spirv-val', '--target-env', 'vulkan1.1', str(out))
        env = dict(os.environ, TEST_DYNAMIC_GPL='1', NX_CLIP_Z_PATCH='0')
        env.pop('NX_R11G11B10_RAW', None)
        run(str(exe), str(spv), str(out), '0', '0', env=env)
        log_env = dict(os.environ, NX_CLIP_Z_PATCH='0')
        for name in ['TEST_DYNAMIC_GPL', 'TEST_PREWARM_MODE']:
            log_env.pop(name, None)
        for mode in range(8):
            log_env['TEST_GPL_LOG_MODE'] = str(mode)
            run(str(exe), str(spv), str(out), '0', '0', env=log_env)
        prewarm_env = dict(os.environ, NX_CLIP_Z_PATCH='0', NX_PIPE_THREADS='1', NX_PIPE_PREWARM='1')
        for name in ['NX_ASYNC_PIPELINES', 'NX_PIPE_CANON', 'NX_PIPE_VERIFY', 'TEST_DYNAMIC_GPL']:
            prewarm_env.pop(name, None)
        for mode in range(4):
            prewarm_env['TEST_PREWARM_MODE'] = str(mode)
            run(str(exe), str(spv), str(out), '0', '0', env=prewarm_env)
        prewarm_env['TEST_PREWARM_MODE'] = '3'
        prewarm_env['NX_PIPE_CANON'] = '0'
        run(str(exe), str(spv), str(out), '0', '0', env=prewarm_env)
        fragment = tmp / 'alpha.spvasm'
        fragment.write_text('''OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint Fragment %main "main" %color
OpExecutionMode %main OriginUpperLeft
OpDecorate %color Location 0
%void = OpTypeVoid
%fn = OpTypeFunction %void
%float = OpTypeFloat 32
%vec4 = OpTypeVector %float 4
%ptr = OpTypePointer Output %vec4
%color = OpVariable %ptr Output
%one = OpConstant %float 1
%value = OpConstantComposite %vec4 %one %one %one %one
%main = OpFunction %void None %fn
%label = OpLabel
OpStore %color %value
OpReturn
OpFunctionEnd
''')
        fragment_spv = tmp / 'alpha.spv'
        fragment_out = tmp / 'alpha-tested.spv'
        run('spirv-as', '--target-env', 'vulkan1.1', str(fragment), '-o', str(fragment_spv))
        env = dict(os.environ, TEST_ALPHA_VARIANT='1')
        env.pop('TEST_UNSUPPORTED_WIDTH', None)
        run(str(exe), str(fragment_spv), str(fragment_out), '0', '0', env=env)
        run('spirv-val', '--target-env', 'vulkan1.1', str(fragment_out))
        env = dict(os.environ, NX_CLIP_Z_PATCH='0', TEST_UNSUPPORTED_WIDTH='1')
        env.pop('TEST_DIAGNOSTIC', None)
        denied = subprocess.run([str(exe), str(spv), str(out), '0', '0'], env=env)
        assert denied.returncode == 86, 'strict mode must reject unsupported line width'
        env['TEST_DIAGNOSTIC'] = '1'
        run(str(exe), str(spv), str(out), '0', '0', env=env)
        print('All 8 configurations, SPIR-V validation and strict/diagnostic width policy passed.')


if __name__ == '__main__':
    main()
