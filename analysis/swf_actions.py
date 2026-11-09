"""Disassemble ActionScript 2 bytecode in an uncompressed SWF/GFX body.

usage: python -I swf_actions.py <file.swf> [--all]
Prints DoAction / DoInitAction blocks (including those inside sprites), with
constant-pool strings resolved. Init actions for caurina.* (Tweener) classes
are skipped unless --all.
"""
import struct
import sys

NAMES = {
    0x04: "nextFrame", 0x05: "prevFrame", 0x06: "play", 0x07: "stop", 0x0A: "add", 0x0B: "subtract", 0x0C: "multiply",
    0x0D: "divide", 0x0E: "equals", 0x0F: "less", 0x10: "and", 0x11: "or", 0x12: "not", 0x17: "pop", 0x18: "toInteger",
    0x1C: "getVariable", 0x1D: "setVariable", 0x20: "setTarget2", 0x21: "stringAdd", 0x22: "getProperty", 0x23: "setProperty",
    0x24: "cloneSprite", 0x25: "removeSprite", 0x26: "trace", 0x34: "getTime", 0x3A: "delete", 0x3B: "delete2",
    0x3C: "defineLocal", 0x3D: "callFunction", 0x3E: "return", 0x3F: "modulo", 0x40: "newObject", 0x41: "defineLocal2",
    0x42: "initArray", 0x43: "initObject", 0x44: "typeof", 0x46: "enumerate", 0x47: "add2", 0x48: "less2", 0x49: "equals2",
    0x4A: "toNumber", 0x4B: "toString", 0x4C: "pushDuplicate", 0x4D: "stackSwap", 0x4E: "getMember", 0x4F: "setMember",
    0x50: "increment", 0x51: "decrement", 0x52: "callMethod", 0x53: "newMethod", 0x54: "instanceOf", 0x55: "enumerate2",
    0x66: "strictEquals", 0x67: "greater", 0x87: "storeRegister", 0x88: "constantPool", 0x8A: "waitForFrame",
    0x8B: "setTarget", 0x8C: "gotoLabel", 0x81: "gotoFrame", 0x83: "getURL", 0x8E: "defineFunction2", 0x94: "with",
    0x96: "push", 0x99: "jump", 0x9A: "getURL2", 0x9B: "defineFunction", 0x9D: "if", 0x9E: "call", 0x9F: "gotoFrame2",
    0x2B: "castOp", 0x2C: "implementsOp", 0x69: "extends", 0x8D: "waitForFrame2",
}


def decode_actions(code, pool_holder, indent="    "):
    i = 0
    out = []
    pool = pool_holder
    while i < len(code):
        op = code[i]
        i += 1
        if op == 0:
            out.append(indent + "end")
            break
        length = 0
        data = b""
        if op >= 0x80:
            length = struct.unpack("<H", code[i:i + 2])[0]
            data = code[i + 2:i + 2 + length]
            i += 2 + length
        name = NAMES.get(op, "op%02X" % op)
        arg = ""
        if op == 0x88:
            n = struct.unpack("<H", data[:2])[0]
            parts = data[2:].split(b"\0")[:n]
            pool[:] = [p.decode("latin1") for p in parts]
            arg = "%d strings" % n
        elif op == 0x96:
            vals = []
            j = 0
            while j < len(data):
                t = data[j]; j += 1
                if t == 0:
                    e = data.index(b"\0", j); vals.append(repr(data[j:e].decode("latin1"))); j = e + 1
                elif t == 1:
                    vals.append(str(struct.unpack("<f", data[j:j + 4])[0])); j += 4
                elif t == 2: vals.append("null")
                elif t == 3: vals.append("undefined")
                elif t == 4: vals.append("r%d" % data[j]); j += 1
                elif t == 5: vals.append("true" if data[j] else "false"); j += 1
                elif t == 6:
                    d = data[j:j + 8]; vals.append(str(struct.unpack("<d", d[4:] + d[:4])[0])); j += 8
                elif t == 7: vals.append(str(struct.unpack("<i", data[j:j + 4])[0])); j += 4
                elif t == 8: vals.append(repr(pool[data[j]]) if data[j] < len(pool) else "c%d" % data[j]); j += 1
                elif t == 9:
                    k = struct.unpack("<H", data[j:j + 2])[0]; vals.append(repr(pool[k]) if k < len(pool) else "c%d" % k); j += 2
                else:
                    vals.append("?%d" % t); break
            arg = ", ".join(vals)
        elif op in (0x99, 0x9D):
            arg = "%+d" % struct.unpack("<h", data[:2])[0]
        elif op in (0x9B, 0x8E):
            e = data.index(b"\0")
            arg = "function %s" % data[:e].decode("latin1")
        elif op == 0x8C:
            arg = data[:-1].decode("latin1")
        elif op == 0x83:
            arg = " ".join(p.decode("latin1") for p in data.split(b"\0") if p)
        elif op == 0x87:
            arg = "r%d" % data[0]
        out.append("%s%s %s" % (indent, name, arg))
    return out


def walk_tags(buf, pos, end, depth, show_all, pool):
    while pos < end:
        code_len = struct.unpack("<H", buf[pos:pos + 2])[0]
        pos += 2
        tag, length = code_len >> 6, code_len & 0x3F
        if length == 0x3F:
            length = struct.unpack("<I", buf[pos:pos + 4])[0]
            pos += 4
        body = buf[pos:pos + length]
        pos += length
        pad = "  " * depth
        if tag == 39:  # DefineSprite
            sid, frames = struct.unpack("<HH", body[:4])
            print("%sDefineSprite id %d (%d frames)" % (pad, sid, frames))
            walk_tags(body, 4, len(body), depth + 1, show_all, pool)
        elif tag == 12:
            print("%sDoAction:" % pad)
            for l in decode_actions(body, pool, pad + "    "):
                print(l)
        elif tag == 59:
            sid = struct.unpack("<H", body[:2])[0]
            lines = decode_actions(body[2:], pool, pad + "    ")
            text = "\n".join(lines)
            if show_all or "caurina" not in text:
                print("%sDoInitAction sprite %d:" % (pad, sid))
                print(text)
        elif tag == 1:
            print("%sShowFrame" % pad)
        elif tag == 43:
            print("%sFrameLabel %s" % (pad, body.split(b"\0")[0].decode("latin1")))
        elif tag == 26 or tag == 70:
            pass
        if tag == 0:
            break


def main():
    data = open(sys.argv[1], "rb").read()
    show_all = "--all" in sys.argv
    nbits = data[8] >> 3
    rect_bytes = (5 + nbits * 4 + 7) // 8
    pos = 8 + rect_bytes + 4
    walk_tags(data, pos, len(data), 0, show_all, [])


if __name__ == "__main__":
    main()
