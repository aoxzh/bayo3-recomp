// Byte-grounded offline disassembly using the same Ryujinx decoder as translation.
// This is a linear decode of the declared program, not a runtime reachability claim.
using System;
using System.IO;
using System.Linq;
using System.Reflection;
using Ryujinx.Graphics.Shader.Decoders;
namespace SpirvHarness {
    static class Disassemble {
        public static int Run(string codePath, string ctrlPath) {
            byte[] code = File.ReadAllBytes(codePath), ctrl = File.ReadAllBytes(ctrlPath);
            int start = BitConverter.ToInt32(ctrl, 0x708), size = BitConverter.ToInt32(ctrl, 0x6f8);
            bool graphics = BitConverter.ToUInt32(code, 0) == 0x12345678;
            int body = start + (graphics ? 0x50 : 0);
            int end = Math.Min(code.Length, start + size);
            Console.WriteLine("code_offset\tprogram_offset\traw_u64\topcode\toperands\tbranch_target");
            for (int off=body; off+8<=end; off+=8) {
                ulong raw = BitConverter.ToUInt64(code, off);
                ulong pc = (ulong)(off-body);
                if ((pc & 31) == 0) { Console.WriteLine($"{off:x}\t{pc:x}\t{raw:x16}\tSCHED\t\t"); continue; }
                InstOp op = InstTable.GetOp(pc,raw);
                string name = op.Emitter?.Method.Name ?? op.Name.ToString();
                Type type = typeof(InstOp).Assembly.GetType("Ryujinx.Graphics.Shader.Decoders.Inst"+name);
                string operands = "";
                if (type != null) {
                    object decoded = Activator.CreateInstance(type, new object[]{raw});
                    operands = string.Join(" ",type.GetProperties(BindingFlags.Public|BindingFlags.Instance).Select(p=>p.Name+"="+p.GetValue(decoded)));
                }
                string target = op.Name is InstName.Bra or InstName.Cal or InstName.Ssy or InstName.Pbk or InstName.Pcnt
                    ? $"0x{op.GetAbsoluteAddress()+(ulong)body:x}" : "";
                Console.WriteLine($"{off:x}\t{pc:x}\t{raw:x16}\t{name}\t{operands}\t{target}");
            }
            return 0;
        }
    }
}
