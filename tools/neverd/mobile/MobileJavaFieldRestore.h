//===- MobileJavaFieldRestore.h - Java field metadata rebuild utility --------===//
#pragma once

#include <string_view>

namespace neverd::mobile::dalvik {
inline constexpr std::string_view JavaFieldRestorePath =
    "rebuild/RestoreFieldFlags.java";
inline constexpr std::string_view JavaFieldRestoreManifestPath =
    "rebuild/runtime-fields.tsv";
inline constexpr std::string_view JavaFieldRestoreSource = R"neverd_java(
import java.io.ByteArrayInputStream;
import java.io.DataInputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.nio.file.StandardCopyOption;
import java.util.LinkedHashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;

/** Applies only the emitted final/synthetic field metadata after javac. */
public final class RestoreFieldFlags {
    private static final int ACC_PUBLIC = 0x0001;
    private static final int ACC_PRIVATE = 0x0002;
    private static final int ACC_PROTECTED = 0x0004;
    private static final int ACC_STATIC = 0x0008;
    private static final int ACC_FINAL = 0x0010;
    private static final int ACC_VOLATILE = 0x0040;
    private static final int ACC_SYNTHETIC = 0x1000;
    private static void require(boolean value, String message) throws IOException {
        if (!value) throw new IOException(message);
    }
    private static final class Reader {
        final byte[] data;
        int at;
        Reader(byte[] data) { this.data = data; }
        void need(long size) throws IOException {
            require(size >= 0 && size <= data.length - at, "Truncated class file");
        }
        int u1() throws IOException { need(1); return data[at++] & 255; }
        int u2() throws IOException { return (u1() << 8) | u1(); }
        long u4() throws IOException { return ((long)u2() << 16) | u2(); }
        void skip(long size) throws IOException { need(size); at += (int)size; }
        String utf() throws IOException {
            int start = at, size = u2(); need(size);
            String value = new DataInputStream(new ByteArrayInputStream(data, start, size + 2)).readUTF();
            at += size; return value;
        }
    }
    private static String utf(String[] pool, int index) throws IOException {
        require(index > 0 && index < pool.length && pool[index] != null, "Invalid class UTF8 reference");
        return pool[index];
    }
    private static void attributes(Reader r, String[] pool) throws IOException {
        int count = r.u2();
        for (int i = 0; i < count; i++) { utf(pool, r.u2()); r.skip(r.u4()); }
    }
    private static void patch(byte[] data, String wantedClass, String wantedName,
                              String wantedType, String mode) throws IOException {
        Reader r = new Reader(data);
        require(r.u4() == 0xcafebabeL, "Invalid class magic"); r.skip(4);
        int count = r.u2(); String[] strings = new String[count]; int[] classes = new int[count];
        for (int i = 1; i < count; i++) {
            int tag = r.u1();
            switch (tag) {
            case 1: strings[i] = r.utf(); break;
            case 3: case 4: r.skip(4); break;
            case 5: case 6: r.skip(8); require(++i < count, "Invalid wide constant slot"); break;
            case 7: classes[i] = r.u2(); break;
            case 8: case 16: case 19: case 20: r.skip(2); break;
            case 9: case 10: case 11: case 12: case 17: case 18: r.skip(4); break;
            case 15: r.skip(3); break;
            default: throw new IOException("Unsupported class constant tag");
            }
        }
        r.skip(2); int self = r.u2();
        require(self > 0 && self < count && classes[self] != 0, "Invalid class identity");
        require(utf(strings, classes[self]).equals(wantedClass), "Compiled class identity differs from manifest");
        r.skip(2); r.skip((long)r.u2() * 2);
        int fields = r.u2(), matched = 0;
        for (int i = 0; i < fields; i++) {
            int offset = r.at, flags = r.u2(); String name = utf(strings, r.u2()), type = utf(strings, r.u2());
            attributes(r, strings);
            if (!name.equals(wantedName) || !type.equals(wantedType)) continue;
            if (mode.equals("private")) {
                int visibility = flags & (ACC_PUBLIC | ACC_PRIVATE | ACC_PROTECTED);
                require((visibility == ACC_PUBLIC || visibility == ACC_PRIVATE) &&
                        (flags & ~(ACC_PUBLIC | ACC_PRIVATE | ACC_FINAL)) == 0,
                        "Private read target must be an ordinary public or restored private instance field");
                data[offset + 1] = (byte)((flags & ~ACC_PUBLIC) | ACC_PRIVATE);
                matched++;
                continue;
            }
            boolean synthetic = mode.equals("final,synthetic");
            require((flags & ACC_VOLATILE) == 0, "Target field has incompatible flags");
            require((flags & ACC_STATIC) == 0 || (synthetic && (flags & ACC_FINAL) != 0),
                    "Static field restoration requires an existing final field and its synthetic bit");
            require((flags & ACC_SYNTHETIC) == 0 || synthetic, "Target field has unexpected synthetic metadata");
            int restored = flags | ACC_FINAL | (synthetic ? ACC_SYNTHETIC : 0);
            data[offset] = (byte)(restored >>> 8); data[offset + 1] = (byte)restored; matched++;
        }
        require(matched == 1, "Manifest does not identify one compiled field");
        int methods = r.u2();
        for (int i = 0; i < methods; i++) { r.skip(2); utf(strings, r.u2()); utf(strings, r.u2()); attributes(r, strings); }
        attributes(r, strings); require(r.at == data.length, "Trailing class bytes");
    }
    public static void main(String[] args) throws Exception {
        require(args.length == 2, "Usage: RestoreFieldFlags <classes-directory> <manifest>");
        Path root = Paths.get(args[0]).toRealPath(); require(Files.isDirectory(root), "Classes root is not a directory");
        Map<Path, byte[]> pending = new LinkedHashMap<Path, byte[]>(); Set<String> seen = new HashSet<String>();
        for (String line : Files.readAllLines(Paths.get(args[1]), StandardCharsets.UTF_8)) {
            if (line.isEmpty()) continue;
            String[] row = line.split("\t", -1); require(row.length == 4, "Malformed field manifest");
            require(row[3].equals("final") || row[3].equals("final,synthetic") || row[3].equals("private"), "Unsupported field restoration flags");
            require(seen.add(row[0] + "\t" + row[1] + "\t" + row[2]), "Duplicate field manifest identity");
            Path file = root.resolve(row[0] + ".class").normalize();
            require(file.startsWith(root) && !file.equals(root), "Field path escapes classes root");
            Path current = root;
            for (Path part : root.relativize(file)) { current = current.resolve(part); require(!Files.isSymbolicLink(current), "Class path contains a symbolic link"); }
            require(Files.isRegularFile(file) && file.toRealPath().startsWith(root), "Compiled class is unavailable");
            byte[] data = pending.get(file); if (data == null) { data = Files.readAllBytes(file); pending.put(file, data); }
            patch(data, row[0], row[1], row[2], row[3]);
        }
        require(!seen.isEmpty(), "Field manifest is empty");
        for (Map.Entry<Path, byte[]> entry : pending.entrySet()) {
            Path temporary = Files.createTempFile(entry.getKey().getParent(), ".neverd-field-flags-", ".class");
            try {
                Files.write(temporary, entry.getValue());
                Files.move(temporary, entry.getKey(), StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE);
            } finally { Files.deleteIfExists(temporary); }
        }
        System.out.println("Restored field metadata: " + seen.size());
    }
}
)neverd_java";
} // namespace neverd::mobile::dalvik
