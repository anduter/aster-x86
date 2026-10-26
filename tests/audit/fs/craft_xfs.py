#!/usr/bin/env python3
"""craft_xfs.py - 生成用于 fs 崩溃/校验审计的 XFS1 v6 镜像。

用法:
    python craft_xfs.py <scenario> <out.img> [--size-mb 8]

scenario:
    magic_bad        超级块 magic 损坏（早期拒绝，内存 FS 必须保持可用）
    version_bad      未知版本号（早期拒绝）
    inode_size_bad   v6 的 inode_size 不是 116（早期拒绝）
    empty            合法 super，但没有任何 inode/卷根（空文件系统）
    orphan           root + 一个父 inode 不存在的文件（清树后失败）
    bad_size         root + size 超过 max_file 的文件（清树后失败）
    bad_data_lba     root + 数据区越界的文件（清树后失败）
    bad_parent_type  root + 父节点是普通文件的子节点（清树后失败）
    type_none        root + used=1/type=0 的幽灵 inode（当前会被接受）
    dup_root         两个卷根占用同一个卷号（整次拒绝）
    nlink_mismatch   root + 文件 nlink=7（应修复并成功挂载）
    good             只有一个卷根的最小合法镜像

退出码: 0 = 已写入; 2 = 参数错误。
"""

import struct
import sys

SECTOR = 512
INODE_MAX = 64
INODE_SECT = 15
DATA_LBA = 16
MAGIC = b"XFS1"

SUPER = struct.Struct("<4s10I")
INODE = struct.Struct("<12I65s3s")

# fs_type_t
NONE, FILE_, DIR, LINK = 0, 1, 2, 3


def mk_super(sector_count, volumes, magic=MAGIC, version=6, inode_size=116):
    vol = bytearray(4 * 32)
    for i, name in enumerate(volumes[:4]):
        raw = name.encode("ascii", "replace")[:31]
        vol[i * 32:i * 32 + len(raw)] = raw
    head = SUPER.pack(
        magic, version, SECTOR, sector_count, INODE_MAX, inode_size,
        1, DATA_LBA, len(volumes), 65536,
        (sector_count - DATA_LBA) * SECTOR,
    )
    blob = head + bytes(vol)
    assert len(blob) <= SECTOR
    return blob + bytes(SECTOR - len(blob))


def mk_inode(used=1, type_=FILE_, parent=0, size=0, data_lba=0, volume=0,
             inode_id=1, nlink=1, name=b""):
    return INODE.pack(used, type_, parent, size, data_lba, volume, 0,
                      0o755, 0, 0, inode_id, nlink,
                      name[:64], b"\x00\x00\x00")


def build(scenario, size_mb=8):
    total = size_mb * 1024 * 1024 // SECTOR
    data = bytearray(total * SECTOR)
    root = mk_inode(type_=DIR, parent=0xFFFFFFFF, volume=0, inode_id=1,
                    nlink=1, name=b"system")
    inodes = [root]

    if scenario == "good":
        superblock = mk_super(total, ["system"])
    elif scenario == "magic_bad":
        superblock = mk_super(total, ["system"], magic=b"XFS0")
    elif scenario == "version_bad":
        superblock = mk_super(total, ["system"], version=7)
    elif scenario == "inode_size_bad":
        superblock = mk_super(total, ["system"], inode_size=108)
    elif scenario == "empty":
        superblock = mk_super(total, [])
        inodes = []
    elif scenario == "volname_empty":
        superblock = mk_super(total, [""])
    elif scenario == "orphan":
        inodes.append(mk_inode(type_=FILE_, parent=5, size=4, data_lba=DATA_LBA,
                               inode_id=2, name=b"lost"))
        superblock = mk_super(total, ["system"])
    elif scenario == "bad_size":
        inodes.append(mk_inode(type_=FILE_, parent=0, size=70000,
                               data_lba=DATA_LBA, inode_id=2, name=b"huge"))
        superblock = mk_super(total, ["system"])
    elif scenario == "bad_data_lba":
        inodes.append(mk_inode(type_=FILE_, parent=0, size=4096,
                               data_lba=total - 1, inode_id=2, name=b"tail"))
        superblock = mk_super(total, ["system"])
    elif scenario == "bad_parent_type":
        inodes.append(mk_inode(type_=FILE_, parent=0, size=0, inode_id=2,
                               name=b"parent"))
        inodes.append(mk_inode(type_=FILE_, parent=1, size=0, inode_id=3,
                               name=b"child"))
        superblock = mk_super(total, ["system"])
    elif scenario == "type_none":
        inodes.append(mk_inode(type_=NONE, parent=0, size=0, inode_id=2,
                               nlink=1, name=b"ghost"))
        superblock = mk_super(total, ["system"])
    elif scenario == "dup_root":
        inodes.append(mk_inode(type_=DIR, parent=0xFFFFFFFF, volume=0,
                               inode_id=2, name=b"dup"))
        superblock = mk_super(total, ["system", "other"])
    elif scenario == "nlink_mismatch":
        inodes.append(mk_inode(type_=FILE_, parent=0, size=4, data_lba=DATA_LBA,
                               inode_id=2, nlink=7, name=b"data"))
        superblock = mk_super(total, ["system"])
    else:
        raise SystemExit("unknown scenario: " + scenario)

    data[0:SECTOR] = superblock
    table = bytearray(INODE_SECT * SECTOR)
    for i, rec in enumerate(inodes):
        assert len(rec) == INODE.size
        table[i * INODE.size:(i + 1) * INODE.size] = rec
    data[1 * SECTOR:1 * SECTOR + len(table)] = table

    if scenario == "nlink_mismatch":
        data[DATA_LBA * SECTOR:DATA_LBA * SECTOR + 4] = b"DATA"
    return bytes(data)


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    scenario, out = argv[1], argv[2]
    size_mb = 8
    if "--size-mb" in argv:
        size_mb = int(argv[argv.index("--size-mb") + 1])
    blob = build(scenario, size_mb)
    with open(out, "wb") as f:
        f.write(blob)
    print("wrote %s (%s, %d bytes)" % (out, scenario, len(blob)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
