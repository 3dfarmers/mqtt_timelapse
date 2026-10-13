#!/usr/bin/env python3
"""Insert timelapse markers into a sliced Bambu .gcode.3mf (github.com/3dfarmers/mqtt_timelapse).

After every Bambu timelapse frame (M971 S11 inside the timelapse_record_flag
conditional, toolhead parked) this adds:

    M400              ; park move finished
    M140 S{bed+1}     ; marker: the board sees bed_target_temper +1 -> phone shutter
    G4 S{dwell}       ; hold the park while the phone takes the photo
    M140 S{bed}       ; restore the bed target

Usage:
    python inject_timelapse_markers.py input.gcode.3mf output.gcode.3mf --dwell 3 [--every 1] [--plate 1]
    python inject_timelapse_markers.py input.gcode.3mf              (inspect only, writes nothing)

Only lines are inserted; every original line stays byte-identical, including
any M104 S1..S5 markers other tools rely on (FarmLoop). The plate's .gcode.md5 is
recomputed. Send the output WITHOUT re-slicing, with Timelapse ON in the print
dialog - otherwise the timelapse conditional (and the marker in it) is skipped.

Standard library only (Python 3.8+).
"""
import argparse
import hashlib
import re
import sys
import zipfile

TAG = b"FL_TIMELAPSE"  # on every inserted line: idempotency check + self-verify

RE_TEMP = re.compile(rb"^M(140|190)\b(.*)$")
RE_TEMP_VAL = re.compile(rb"[SR](-?\d+(?:\.\d+)?)")
RE_M971 = re.compile(rb"^M971\b(.*)$")
RE_PARAM = lambda p: re.compile(rb"\b" + p + rb"(-?\d+)")
RE_S = RE_PARAM(b"S")
RE_C = RE_PARAM(b"C")
RE_J = RE_PARAM(b"J")
RE_XY_MOVE = re.compile(rb"^G0?[01]\b.*\b[XY]-?\d")
RE_DIGITAL = re.compile(rb"^M104\s+S([1-5])\s*$")
RE_HEADER = lambda key: re.compile(rb"^;\s*" + key + rb"\s*[=:]\s*(.+?)\s*$", re.M)


def code_of(line):
    """Uppercased G-code part of a line, comment and line ending removed."""
    return line.split(b";", 1)[0].strip().upper()


def fmt_temp(v):
    return str(int(round(v)))


def die(msg):
    sys.stdout.flush()   # keep the inspection output above the error when piped
    print("\nERROR: " + msg, file=sys.stderr)
    sys.exit(1)


def header_value(gcode, key):
    m = RE_HEADER(key).search(gcode)
    return m.group(1).decode("utf-8", "replace") if m else None


def scan(lines, args):
    """One pass over the G-code. Returns the insertion plan and what was seen."""
    found = {
        "layers": 0,
        "frames_in_block": [],     # (line index, C param, parked)
        "frames_outside": [],      # M971 S11 not inside the timelapse conditional
        "other_m971": 0,
        "bed_values": [],          # distinct bed targets in order of appearance
        "digital": 0,
        "plus_delta_changes": [],  # real bed changes that look like a marker
    }
    plan = []                      # dicts: index, layer, bed, settle

    bed = None
    bed_changed_at = -1            # line index of the last bed target CHANGE
    first_layer_at = None
    last_frame_at = None           # previous timelapse frame (inserted or not)
    layer = 0

    judge_pending = False          # saw "M1002 judge_flag timelapse_record_flag"
    block_depth = 0                # >0: inside M622 J1 ... M623 of that flag
    parked = False
    candidates = 0

    for i, line in enumerate(lines):
        stripped = line.lstrip()
        if stripped.startswith(b"; CHANGE_LAYER"):
            layer += 1
            found["layers"] += 1
            if first_layer_at is None:
                first_layer_at = i
            continue
        code = code_of(line)
        if not code:
            continue

        m = RE_TEMP.match(code)
        if m:
            v = RE_TEMP_VAL.search(m.group(2))
            if v:
                new = float(v.group(1))
                if bed is None or abs(new - bed) > 0.01:
                    if (bed is not None and first_layer_at is not None
                            and abs(new - (bed + args.delta)) < 0.01):
                        found["plus_delta_changes"].append((i, layer, bed, new))
                    bed_changed_at = i
                    if new not in found["bed_values"]:
                        found["bed_values"].append(new)
                bed = new
            continue

        if RE_DIGITAL.match(code):
            found["digital"] += 1
            continue

        if code.startswith(b"M1002") and b"TIMELAPSE_RECORD_FLAG" in code:
            judge_pending = True
            continue

        if code.startswith(b"M622") and not code.startswith(b"M622.1"):
            if block_depth:
                block_depth += 1                     # nested conditional
            elif judge_pending:
                j = RE_J.search(code)
                if j and j.group(1) == b"1":
                    block_depth = 1
                    parked = False
            judge_pending = False
            continue

        if code.startswith(b"M623"):
            if block_depth:
                block_depth -= 1
            continue

        if block_depth and RE_XY_MOVE.match(code):
            parked = True
            continue

        m = RE_M971.match(code)
        if m:
            s = RE_S.search(m.group(1))
            if not s or s.group(1) != b"11":
                found["other_m971"] += 1
                continue
            c = RE_C.search(m.group(1))
            c = c.group(1).decode() if c else "?"
            if not block_depth:
                found["frames_outside"].append(i)
                continue
            found["frames_in_block"].append((i, c, parked))

            # Settle when the bed target changed since the previous frame
            # (first frame: since layer 1 started). The board only takes a
            # value as its base after it has been held ~1 s, so a marker right
            # after a change (first layer -> second layer) could be read as
            # "+1 from the old value" and missed.
            since = last_frame_at if last_frame_at is not None else (first_layer_at or 0)
            settle = args.settle if bed_changed_at > since else 0
            last_frame_at = i

            k = candidates
            candidates += 1
            if not parked and not args.allow_unparked:
                continue
            if k % args.every:
                continue
            plan.append({"index": i, "layer": layer, "bed": bed, "settle": settle})

    return plan, found


def marker_block(bed, settle, args, eol):
    out = [b"; " + TAG + b" marker (github.com/3dfarmers/mqtt_timelapse)",
           b"M400 ; " + TAG + b" park move finished"]
    if settle:
        out.append(b"G4 S%d ; %s bed target just changed: let it register first" % (settle, TAG))
    out += [
        b"M140 S%s ; %s marker: bed target +%d" % (fmt_temp(bed + args.delta).encode(), TAG, args.delta),
        b"G4 S%d ; %s hold the park for the photo" % (args.dwell, TAG),
        b"M140 S%s ; %s restore bed target" % (fmt_temp(bed).encode(), TAG),
    ]
    return [l + eol for l in out]


def main():
    ap = argparse.ArgumentParser(
        description="Insert timelapse markers (bed target +1) after Bambu's timelapse park.")
    ap.add_argument("input", help="sliced .gcode.3mf from Bambu Studio")
    ap.add_argument("output", nargs="?", help="output .gcode.3mf (omit to inspect only)")
    ap.add_argument("--dwell", type=int, default=3,
                    help="seconds to hold the park per photo (G4 S, whole seconds; default 3)")
    ap.add_argument("--every", type=int, default=1, help="marker on every Nth layer (default 1)")
    ap.add_argument("--plate", type=int, default=1, help="plate number to process (default 1)")
    ap.add_argument("--delta", type=int, default=1,
                    help="marker step in C, must equal tl_marker_delta on the board (default 1)")
    ap.add_argument("--settle", type=int, default=3,
                    help="extra seconds before a marker when the bed target changed since the "
                         "previous layer, 0 = off (default 3)")
    ap.add_argument("--allow-unparked", action="store_true",
                    help="also mark frames with no park move before them (Traditional timelapse)")
    args = ap.parse_args()

    if args.dwell < 1 or args.every < 1 or args.delta < 1 or args.settle < 0:
        die("--dwell, --every and --delta must be >= 1, --settle >= 0")
    if args.output and args.output == args.input:
        die("output must be a different file than the input")

    gcode_name = "Metadata/plate_%d.gcode" % args.plate
    md5_name = gcode_name + ".md5"

    try:
        zin = zipfile.ZipFile(args.input)
    except (OSError, zipfile.BadZipFile) as e:
        die("cannot open %s as a .3mf (zip): %s" % (args.input, e))
    with zin:
        names = zin.namelist()
        if gcode_name not in names:
            plates = sorted(n for n in names if re.fullmatch(r"Metadata/plate_\d+\.gcode", n))
            die("%s has no %s. G-code plates in this file: %s\n"
                "Is this a SLICED file (.gcode.3mf, exported after slicing)?"
                % (args.input, gcode_name, ", ".join(plates) or "none"))
        gcode = zin.read(gcode_name)
        old_md5 = zin.read(md5_name) if md5_name in names else None
        slice_info = zin.read("Metadata/slice_info.config") if "Metadata/slice_info.config" in names else b""

        # ------------------------------------------------------------- inspect
        crlf = gcode.count(b"\r\n")
        eol = b"\r\n" if crlf > (gcode.count(b"\n") - crlf) else b"\n"
        lines = gcode.splitlines(keepends=True)
        model = header_value(gcode, b"printer_model")
        if not model:
            m = re.search(rb'key="printer_model_id"\s+value="([^"]*)"', slice_info)
            model = m.group(1).decode() if m else "unknown"
        tl_type = header_value(gcode, b"timelapse_type")
        total_hdr = header_value(gcode, b"total layer number")

        print("Input:   %s (%d zip entries)" % (args.input, len(names)))
        print("Plate:   %s, %.1f MB, %d lines, line endings %s"
              % (gcode_name, len(gcode) / 1e6, len(lines), "CRLF" if eol == b"\r\n" else "LF"))
        if old_md5 is None:
            print("MD5:     no %s in this file (nothing to update)" % md5_name)
        else:
            ok = old_md5.strip().upper() == hashlib.md5(gcode).hexdigest().upper().encode()
            print("MD5:     %s present, %s" % (md5_name, "matches the G-code" if ok
                                               else "does NOT match the G-code as shipped"))
        print("Printer: %s | timelapse_type: %s" % (model, {
            "0": "0 (Traditional)", "1": "1 (Smooth)"}.get(tl_type, tl_type or "not in header")))

        if TAG in gcode:
            die("this file already contains %s markers - run the script on the original slice"
                % TAG.decode())

        plan, found = scan(lines, args)
        frames = found["frames_in_block"]
        parked = sum(1 for f in frames if f[2])
        print("Layers:  %d (; CHANGE_LAYER)%s" % (
            found["layers"], ", header says %s" % total_hdr if total_hdr else ""))
        print("Frames:  %d x M971 S11 inside the timelapse conditional (%d with a park move before), "
              "%d outside it (skipped)" % (len(frames), parked, len(found["frames_outside"])))
        if frames:
            cs = sorted({f[1] for f in frames})
            print("         M971 C values: %s" % ", ".join(cs))
        if found["other_m971"]:
            print("         %d other M971 lines (not S11, left alone)" % found["other_m971"])
        print("Bed:     targets seen in order: %s" % (
            ", ".join(fmt_temp(v) for v in found["bed_values"]) or "none"))
        print("M104:   %d low nozzle-target lines (M104 S1..S5, FarmLoop markers) left untouched" % found["digital"])

        if not frames:
            die("no Bambu timelapse frame (M971 S11 inside the timelapse_record_flag conditional) "
                "in %s.\nOther printer model, or a template this script does not know. "
                "Not guessing a position - send this file to firmware." % gcode_name)
        if parked == 0 and not args.allow_unparked:
            die("timelapse frames found, but none has a park move before it. This looks like "
                "Traditional timelapse (the head stays over the part).\nSet Timelapse to Smooth "
                "in Bambu Studio (Others > Special mode), re-slice, and run this again "
                "(or pass --allow-unparked).")
        if parked < len(frames) and not args.allow_unparked:
            print("WARNING: %d frame(s) without a park move are skipped" % (len(frames) - parked))
        for i, lay, old, new in found["plus_delta_changes"]:
            print("WARNING: line %d (layer %d) changes the bed target %s -> %s, exactly +%d. "
                  "The board will read it as a marker and fire an extra photo there. "
                  "Use --delta 2 and `tl delta 2` on the board to avoid it."
                  % (i + 1, lay, fmt_temp(old), fmt_temp(new), args.delta))

        no_bed = [p for p in plan if not p["bed"]]
        plan = [p for p in plan if p["bed"]]
        if no_bed:
            print("WARNING: %d frame(s) skipped: no bed target known yet, or it is 0" % len(no_bed))
        if not plan:
            die("nothing to insert")

        # -------------------------------------------------------------- insert
        out_lines = []
        inserted_at = {}           # output index of the first inserted line -> plan entry
        added_eol = b""
        cursor = 0
        for p in plan:
            out_lines.extend(lines[cursor:p["index"] + 1])
            src = lines[p["index"]]
            # The frame line normally ends with eol; if it is the very last line
            # of the file and has none, give it one before the block.
            if not src.endswith((b"\n", b"\r")):
                out_lines[-1] = src + eol
                added_eol = eol
            inserted_at[len(out_lines)] = p
            out_lines.extend(marker_block(p["bed"], p["settle"], args, eol))
            cursor = p["index"] + 1
        out_lines.extend(lines[cursor:])
        new_gcode = b"".join(out_lines)

        # Self-check: dropping our lines must give back the original exactly.
        kept = b"".join(l for l in out_lines if TAG not in l)
        if kept != gcode + added_eol:
            die("internal check failed: original lines changed. Nothing written.")
        if len(re.findall(rb"(?m)^M104\s+S[1-5]\s*$", new_gcode)) != \
                len(re.findall(rb"(?m)^M104\s+S[1-5]\s*$", gcode)):
            die("internal check failed: M104 S1..S5 line count changed. Nothing written.")

        # ---------------------------------------------------------------- diff
        print("\nFirst %d insertion(s), 5 lines of context:" % min(3, len(inserted_at)))
        shown = 0
        for start, p in inserted_at.items():
            if shown == 3:
                break
            shown += 1
            n = len(marker_block(p["bed"], p["settle"], args, eol))
            print("--- layer %d, after original line %d" % (p["layer"], p["index"] + 1))
            for j in range(max(0, start - 5), min(len(out_lines), start + n + 5)):
                mark = "+" if start <= j < start + n else " "
                print("%s %s" % (mark, out_lines[j].rstrip(b"\r\n").decode("utf-8", "replace")))

        settles = [p for p in plan if p["settle"]]
        extra = len(plan) * args.dwell + sum(p["settle"] for p in plan)
        print("\nResult:  %d layers, %d markers inserted (%s), %d settle pause(s)%s"
              % (found["layers"], len(plan),
                 "every frame" if args.every == 1 else "1 in %d frames" % args.every,
                 len(settles),
                 " at layer " + ", ".join(str(p["layer"]) for p in settles) if settles else ""))
        print("         extra print time: %d x %d s%s = %d s (%.1f min)"
              % (len(plan), args.dwell,
                 " + %d s settle" % sum(p["settle"] for p in settles) if settles else "",
                 extra, extra / 60.0))

        if not args.output:
            print("\nInspect only (no output file given). Nothing written.")
            return

        # --------------------------------------------------------------- write
        new_md5 = None
        if old_md5 is not None:
            digest = hashlib.md5(new_gcode).hexdigest()
            stripped = old_md5.strip()
            digest = digest.lower() if stripped and stripped == stripped.lower() else digest.upper()
            new_md5 = digest.encode() + old_md5[len(stripped):]   # keep any trailing newline

        with zipfile.ZipFile(args.output, "w", zipfile.ZIP_DEFLATED, allowZip64=True) as zout:
            for info in zin.infolist():
                data = zin.read(info)
                zi = zipfile.ZipInfo(info.filename, date_time=info.date_time)
                zi.external_attr = info.external_attr
                zi.compress_type = info.compress_type
                if info.filename == gcode_name:
                    data, zi.compress_type = new_gcode, zipfile.ZIP_DEFLATED
                elif info.filename == md5_name:
                    data, zi.compress_type = new_md5, zipfile.ZIP_DEFLATED
                zout.writestr(zi, data)

    print("\nWritten: %s" % args.output)
    if new_md5 is not None:
        print("MD5:     %s = %s" % (md5_name, new_md5.strip().decode()))
    print("""
Before printing:
  1. Send %s WITHOUT re-slicing: open it in Bambu Studio and print from the
     preview, or copy it to the SD card. Re-slicing drops the markers.
  2. Switch Timelapse ON in the print dialog. With it off the printer skips the
     whole timelapse block, and the markers with it.
  3. Board: phone paired, `tl status` shows a phone link and the printer is connected.
     `tl delta` on the board must be %d (same as --delta).""" % (args.output, args.delta))


if __name__ == "__main__":
    main()
