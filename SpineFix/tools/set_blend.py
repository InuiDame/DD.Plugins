#!/usr/bin/env python3
"""读写 Spine 2.1 二进制骨骼（.skel）里每个切片的混合字节。

《暗黑地牢》的 spine-c 是 2.1，slot 记录的最后有一个 additive 字节。原版只当 0/1 用，
SpineFix 插件把它扩成混合模式：

    0 = 正常   1 = 加法   2 = 正片叠底   3 = 滤色

这个脚本只改这一个字节，文件长度不变，其余内容原样保留。

用法：
    python set_blend.py --list a.skel              # 只报告，不写
    python set_blend.py --set head=2 a.skel        # 预览要改成什么
    python set_blend.py --set head=2 a.skel --write
    python set_blend.py --all 2 a.skel --write

--write 之前会在同目录留一份 <文件名>.blendbak。
布局见 knowledge/dd1/spine.md 的「.skel 二进制布局」。
"""

from __future__ import annotations

import argparse
import shutil
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

BLEND_NAMES = {0: "normal", 1: "additive", 2: "multiply", 3: "screen"}


class ParseError(Exception):
    pass


class Reader:
    def __init__(self, data: bytes) -> None:
        self.data = data
        self.pos = 0

    def u8(self) -> int:
        if self.pos >= len(self.data):
            raise ParseError(f"读取越界：偏移 {self.pos}")
        value = self.data[self.pos]
        self.pos += 1
        return value

    def raw(self, count: int) -> bytes:
        if self.pos + count > len(self.data):
            raise ParseError(f"读取越界：偏移 {self.pos} 需要 {count} 字节")
        chunk = self.data[self.pos : self.pos + count]
        self.pos += count
        return chunk

    def varint(self) -> int:
        """libgdx 的 readInt(true)：小端 7 位一组，最多 5 字节。"""
        result = 0
        for index in range(5):
            byte = self.u8()
            result |= (byte & 0x7F) << (7 * index)
            if not byte & 0x80:
                return result
        return result

    def string(self) -> str | None:
        length = self.varint()
        if length == 0:
            return None
        if length == 1:
            return ""
        raw = self.raw(length - 1)
        try:
            return raw.decode("utf-8")
        except UnicodeDecodeError as error:
            raise ParseError(f"偏移 {self.pos - len(raw)} 的字符串不是 UTF-8") from error

    def float32(self) -> float:
        return struct.unpack(">f", self.raw(4))[0]


@dataclass
class Slot:
    name: str | None
    bone_index: int
    color: int
    attachment: str | None
    blend: int
    blend_offset: int


@dataclass
class Skeleton:
    version: str | None = None
    bone_names: list[str | None] = field(default_factory=list)
    slots: list[Slot] = field(default_factory=list)


def parse_slots(data: bytes) -> Skeleton:
    reader = Reader(data)
    skeleton = Skeleton()

    reader.string()  # hash
    skeleton.version = reader.string()
    reader.float32()  # width
    reader.float32()  # height

    nonessential = reader.u8()
    if nonessential:
        reader.string()  # imagesPath

    bone_count = reader.varint()
    if bone_count > 0x400:
        raise ParseError(f"骨骼数量不合理：{bone_count}，可能不是 2.1 二进制骨骼")
    for _ in range(bone_count):
        name = reader.string()
        skeleton.bone_names.append(name)
        reader.varint()  # parentIndex + 1
        for _ in range(6):  # x y scaleX scaleY rotation length
            reader.float32()
        reader.raw(4)  # flipX flipY inheritScale inheritRotation
        if nonessential:
            reader.raw(4)  # color

    ik_count = reader.varint()
    if ik_count > 0x100:
        raise ParseError(f"IK 数量不合理：{ik_count}")
    for _ in range(ik_count):
        reader.string()
        bones = reader.varint()
        for _ in range(bones):
            reader.varint()
        reader.varint()  # target
        reader.float32()  # mix
        reader.raw(1)  # bendDir

    slot_count = reader.varint()
    if slot_count > 0x400:
        raise ParseError(f"切片数量不合理：{slot_count}")

    for _ in range(slot_count):
        name = reader.string()
        bone_index = reader.varint()
        color = struct.unpack("<I", reader.raw(4))[0]
        attachment = reader.string()
        offset = reader.pos
        blend = reader.u8()
        if bone_index >= max(bone_count, 1):
            raise ParseError(f"切片 {name!r} 的骨骼下标 {bone_index} 超出 {bone_count}，解析已经错位")

        skeleton.slots.append(Slot(name, bone_index, color, attachment, blend, offset))

    return skeleton


def describe(skeleton: Skeleton) -> None:
    print(f"version：{skeleton.version!r}  骨骼 {len(skeleton.bone_names)}  切片 {len(skeleton.slots)}")
    width = max((len(slot.name or "") for slot in skeleton.slots), default=4)
    for slot in skeleton.slots:
        color = slot.color
        blend = slot.blend
        label = BLEND_NAMES.get(blend, f"unknown({blend})")
        print(
            f"  {(slot.name or '<null>'):<{width}}  bone={slot.bone_index:<3} "
            f"color=#{color:08X}  attachment={slot.attachment!r}  blend={blend} ({label})  @0x{slot.blend_offset:X}"
        )


def main() -> int:
    parser = argparse.ArgumentParser(description="读写 .skel 里切片的混合字节")
    parser.add_argument("files", nargs="+", type=Path)
    parser.add_argument("--list", action="store_true", help="只报告（默认行为）")
    parser.add_argument("--set", action="append", default=[], metavar="SLOT=MODE", help="把某个切片设为某个模式，可重复")
    parser.add_argument("--all", type=int, metavar="MODE", help="把所有切片设为同一个模式")
    parser.add_argument("--write", action="store_true", help="真的写回文件；不给时只预览")
    args = parser.parse_args()

    if not args.set and args.all is None:
        args.list = True

    failed = False
    for path in args.files:
        try:
            data = bytearray(path.read_bytes())
            skeleton = parse_slots(data)
        except (OSError, ParseError) as error:
            print(f"{path}: 跳过（{error}）")
            failed = True
            continue

        print(f"== {path}")
        if args.list:
            describe(skeleton)

        changes: list[tuple[Slot, int]] = []
        if args.all is not None:
            if args.all not in BLEND_NAMES:
                print(f"{path}: --all 只接受 0..3")
                failed = True
                continue
            changes = [(slot, args.all) for slot in skeleton.slots]

        for item in args.set:
            if "=" not in item:
                print(f"{path}: --set 需要 SLOT=MODE，收到 {item!r}")
                failed = True
                continue
            name, _, mode_text = item.rpartition("=")
            try:
                mode = int(mode_text)
            except ValueError:
                print(f"{path}: 模式 {mode_text!r} 不是数字")
                failed = True
                continue
            if mode not in BLEND_NAMES:
                print(f"{path}: 模式只接受 0..3")
                failed = True
                continue
            matched = [slot for slot in skeleton.slots if slot.name == name]
            if not matched:
                print(f"{path}: 找不到切片 {name!r}")
                failed = True
                continue
            changes.extend((slot, mode) for slot in matched)

        if not changes:
            continue

        for slot, mode in changes:
            if slot.blend == mode:
                print(f"  {slot.name}: 已经是 {mode}，跳过")
                continue
            print(f"  {slot.name}: {slot.blend} -> {mode}")
            data[slot.blend_offset] = mode

        if not args.write:
            print(f"  （预览）加 --write 才会写回 {path}")
            continue

        backup = path.with_name(path.name + ".blendbak")
        if not backup.exists():
            shutil.copy2(path, backup)
            print(f"  备份：{backup.name}")
        path.write_bytes(data)
        print(f"  已写入 {path}")

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
