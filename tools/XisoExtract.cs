// Xbox / Xbox 360 disc image (XDVDFS) extractor.
// Handles plain XISO and XGD2/XGD3 "redump" images (game partition at an offset).
// Usage (via tools/extract_iso.ps1): XisoExtract.Run(iso, outdir)
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

public static class XisoExtract
{
    const long Sector = 2048;
    static readonly long[] PartitionOffsets = { 0, 0xFD90000, 0x2080000, 0x18300000 };
    static readonly byte[] Magic = Encoding.ASCII.GetBytes("MICROSOFT*XBOX*MEDIA");

    static FileStream iso;
    static long baseOffset;
    static long total, done;

    static byte[] Read(long offset, int count)
    {
        var buf = new byte[count];
        iso.Seek(offset, SeekOrigin.Begin);
        int n = 0;
        while (n < count) { int r = iso.Read(buf, n, count - n); if (r <= 0) break; n += r; }
        return buf;
    }

    static bool IsMagic(long offset)
    {
        var b = Read(offset, Magic.Length);
        for (int i = 0; i < Magic.Length; i++) if (b[i] != Magic[i]) return false;
        return true;
    }

    struct Entry { public string Name; public uint Sector, Size; public bool Dir; }

    static List<Entry> ReadDir(uint sector, uint size)
    {
        var list = new List<Entry>();
        if (size == 0) return list;
        var table = Read(baseOffset + sector * Sector, (int)size);
        var stack = new Stack<int>();
        var seen = new HashSet<int>();
        stack.Push(0);
        while (stack.Count > 0)
        {
            int o = stack.Pop();
            if (o + 14 > table.Length || !seen.Add(o)) continue;
            ushort left = BitConverter.ToUInt16(table, o), right = BitConverter.ToUInt16(table, o + 2);
            if (left == 0xFFFF && right == 0xFFFF) continue;
            uint start = BitConverter.ToUInt32(table, o + 4), len = BitConverter.ToUInt32(table, o + 8);
            byte attr = table[o + 12], nameLen = table[o + 13];
            if (o + 14 + nameLen > table.Length) continue;
            list.Add(new Entry { Name = Encoding.ASCII.GetString(table, o + 14, nameLen), Sector = start, Size = len, Dir = (attr & 0x10) != 0 });
            if (left != 0 && left != 0xFFFF) stack.Push(left * 4);
            if (right != 0 && right != 0xFFFF) stack.Push(right * 4);
        }
        return list;
    }

    static void Walk(uint sector, uint size, string outDir, bool countOnly)
    {
        foreach (var e in ReadDir(sector, size))
        {
            string path = Path.Combine(outDir, e.Name);
            if (e.Dir) { if (!countOnly) Directory.CreateDirectory(path); Walk(e.Sector, e.Size, path, countOnly); continue; }
            if (countOnly) { total += e.Size; continue; }
            using (var outF = File.Create(path))
            {
                long remaining = e.Size, pos = baseOffset + e.Sector * Sector;
                var buf = new byte[1 << 20];
                iso.Seek(pos, SeekOrigin.Begin);
                while (remaining > 0)
                {
                    int n = iso.Read(buf, 0, (int)Math.Min(buf.Length, remaining));
                    if (n <= 0) throw new Exception("Unexpected end of image reading " + path);
                    outF.Write(buf, 0, n);
                    remaining -= n; done += n;
                }
            }
        }
    }

    // Title ID from the disc's default.xex (execution info header), or 0.
    public static uint TitleId(string isoPath)
    {
        using (iso = new FileStream(isoPath, FileMode.Open, FileAccess.Read, FileShare.Read))
        {
            baseOffset = -1;
            foreach (var p in PartitionOffsets)
                if (p + 0x10000 + 20 < iso.Length && IsMagic(p + 0x10000)) { baseOffset = p; break; }
            if (baseOffset < 0) return 0;
            var vd = Read(baseOffset + 0x10000, 0x20);
            foreach (var e in ReadDir(BitConverter.ToUInt32(vd, 0x14), BitConverter.ToUInt32(vd, 0x18)))
            {
                if (e.Dir || !e.Name.Equals("default.xex", StringComparison.OrdinalIgnoreCase)) continue;
                var h = Read(baseOffset + e.Sector * Sector, (int)Math.Min(e.Size, 0x4000u));
                Func<int, uint> be = o => (uint)(h[o] << 24 | h[o + 1] << 16 | h[o + 2] << 8 | h[o + 3]);
                if (h[0] != 'X' || h[1] != 'E' || h[2] != 'X' || h[3] != '2') return 0;
                uint count = be(0x14);
                for (int i = 0; i < count && 0x18 + i * 8 + 8 <= h.Length; i++)
                    if (be(0x18 + i * 8) == 0x00040006) { int v = (int)be(0x1C + i * 8); return v + 16 <= h.Length ? be(v + 12) : 0; }
            }
            return 0;
        }
    }

    public static int Run(string isoPath, string outDir)
    {
        using (iso = new FileStream(isoPath, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20))
        {
            baseOffset = -1;
            foreach (var p in PartitionOffsets)
                if (p + 0x10000 + 20 < iso.Length && IsMagic(p + 0x10000)) { baseOffset = p; break; }
            if (baseOffset < 0) throw new Exception("No XDVDFS volume found (not an Xbox disc image?)");
            var vd = Read(baseOffset + 0x10000, 0x20);
            uint rootSector = BitConverter.ToUInt32(vd, 0x14), rootSize = BitConverter.ToUInt32(vd, 0x18);
            Console.WriteLine("partition=0x{0:X} root sector={1} size={2}", baseOffset, rootSector, rootSize);
            Directory.CreateDirectory(outDir);
            Walk(rootSector, rootSize, outDir, true);
            Console.WriteLine("extracting {0:N0} MB", total >> 20);
            Walk(rootSector, rootSize, outDir, false);
            Console.WriteLine("done {0:N0} MB", done >> 20);
        }
        return 0;
    }
}
