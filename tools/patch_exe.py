"""
由 Le04.exe 生成 Le04_Multi.exe：
  - 追加一个 .mult 节，放新的导入表（原有导入 + le04multi.dll!MultiVersion），使游戏启动时加载 le04multi.dll；
  - 去掉 ASLR（DYNAMIC_BASE），基址固定 0x400000，与反汇编地址一致；
  - 打开大地址（LARGE_ADDRESS_AWARE）：64 位系统上能用 4GB 地址空间（回滚要多保留内存；原版练习重开会漏图像）；
  - 清掉绑定导入目录。
用法：python patch_exe.py [src.exe] [dst.exe]
"""
import struct, sys
import pefile

SRC = "Le04.exe"          # 默认：当前目录（游戏目录）
DST = "Le04_Multi.exe"
DLL = b"le04multi.dll"
FUNC = b"MultiVersion"


def align(v, a):
    return (v + a - 1) // a * a


def main(src=SRC, dst=DST):
    pe = pefile.PE(src)
    data = bytearray(open(src, "rb").read())
    fa = pe.OPTIONAL_HEADER.FileAlignment
    sa = pe.OPTIONAL_HEADER.SectionAlignment
    last = pe.sections[-1]
    new_rva = align(last.VirtualAddress + last.Misc_VirtualSize, sa)
    new_raw = align(len(data), fa)

    # 原导入描述符
    imp_dir = pe.OPTIONAL_HEADER.DATA_DIRECTORY[1]
    descs = []
    off = pe.get_offset_from_rva(imp_dir.VirtualAddress)
    while True:
        d = data[off:off + 20]
        if d == b"\0" * 20:
            break
        descs.append(bytes(d))
        off += 20

    # 新节布局：[描述符表 (n+2)*20] [ILT 2*4] [IAT 2*4] [hint/name] [dll 名]
    n = len(descs)
    tbl = (n + 2) * 20
    ilt = tbl
    iat = ilt + 8
    hint = iat + 8
    name = align(hint + 2 + len(FUNC) + 1, 2)
    size = name + len(DLL) + 1
    sec = bytearray(align(size, fa))
    for i, d in enumerate(descs):
        sec[i * 20:i * 20 + 20] = d
    struct.pack_into("<IIIII", sec, n * 20, new_rva + ilt, 0, 0, new_rva + name, new_rva + iat)
    struct.pack_into("<II", sec, ilt, new_rva + hint, 0)
    struct.pack_into("<II", sec, iat, new_rva + hint, 0)
    struct.pack_into("<H", sec, hint, 0)
    sec[hint + 2:hint + 2 + len(FUNC)] = FUNC
    sec[name:name + len(DLL)] = DLL

    # 节表追加一项
    nt = pe.DOS_HEADER.e_lfanew
    nsec_off = nt + 6
    opt_off = nt + 24
    sh_off = opt_off + pe.FILE_HEADER.SizeOfOptionalHeader + len(pe.sections) * 40
    if sh_off + 40 > pe.sections[0].PointerToRawData:
        raise SystemExit("no room for another section header")
    hdr = struct.pack("<8sIIIIIIHHI", b".mult", len(sec), new_rva, len(sec), new_raw, 0, 0, 0, 0,
                      0x40000000 | 0x80000000 | 0x00000040)   # 可读可写、已初始化数据（加载器要写 IAT）
    data[sh_off:sh_off + 40] = hdr
    struct.pack_into("<H", data, nsec_off, len(pe.sections) + 1)

    # 可选头：SizeOfImage、导入目录、去 ASLR、清绑定导入 / IAT 目录（新 IAT 在 .mult 里）
    struct.pack_into("<I", data, opt_off + 56, align(new_rva + len(sec), sa))
    dd = opt_off + 96
    struct.pack_into("<II", data, dd + 1 * 8, new_rva, tbl)
    struct.pack_into("<II", data, dd + 11 * 8, 0, 0)
    ch_off = nt + 22   # FILE_HEADER.Characteristics
    struct.pack_into("<H", data, ch_off, struct.unpack_from("<H", data, ch_off)[0] | 0x20)
    dllch_off = opt_off + 70
    dllch = struct.unpack_from("<H", data, dllch_off)[0]
    struct.pack_into("<H", data, dllch_off, dllch & ~0x40)

    data = data[:new_raw] + b"\0" * (new_raw - len(data)) if len(data) < new_raw else data[:new_raw]
    data += sec
    open(dst, "wb").write(data)
    chk = pefile.PE(dst)
    print("wrote", dst, "imports:", [e.dll.decode() for e in chk.DIRECTORY_ENTRY_IMPORT],
          "dllchar %04X" % chk.OPTIONAL_HEADER.DllCharacteristics, "characteristics %04X" % chk.FILE_HEADER.Characteristics)


if __name__ == "__main__":
    main(*sys.argv[1:])
